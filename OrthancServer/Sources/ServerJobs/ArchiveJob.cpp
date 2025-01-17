/**
 * Orthanc - A Lightweight, RESTful DICOM Store
 * Copyright (C) 2012-2016 Sebastien Jodogne, Medical Physics
 * Department, University Hospital of Liege, Belgium
 * Copyright (C) 2017-2021 Osimis S.A., Belgium
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * In addition, as a special exception, the copyright holders of this
 * program give permission to link the code of its release with the
 * OpenSSL project's "OpenSSL" library (or with modified versions of it
 * that use the same license as the "OpenSSL" library), and distribute
 * the linked executables. You must obey the GNU General Public License
 * in all respects for all of the code used other than "OpenSSL". If you
 * modify file(s) with this exception, you may extend this exception to
 * your version of the file(s), but you are not obligated to do so. If
 * you do not wish to do so, delete this exception statement from your
 * version. If you delete this exception statement from all source files
 * in the program, then also delete it here.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 **/


#include "../PrecompiledHeadersServer.h"
#include "ArchiveJob.h"

#include "../../../OrthancFramework/Sources/Cache/SharedArchive.h"
#include "../../../OrthancFramework/Sources/Compression/HierarchicalZipWriter.h"
#include "../../../OrthancFramework/Sources/DicomParsing/DicomDirWriter.h"
#include "../../../OrthancFramework/Sources/DicomParsing/FromDcmtkBridge.h"
#include "../../../OrthancFramework/Sources/Logging.h"
#include "../../../OrthancFramework/Sources/OrthancException.h"
#include "../OrthancConfiguration.h"
#include "../ServerContext.h"

#include <stdio.h>
#include <thread>
#include <atomic>

#if defined(_MSC_VER)
#define snprintf _snprintf
#endif

static const uint64_t MEGA_BYTES = 1024 * 1024;
static const uint64_t GIGA_BYTES = 1024 * 1024 * 1024;

static const char* const MEDIA_IMAGES_FOLDER = "IMAGES"; 
static const char* const KEY_DESCRIPTION = "Description";
static const char* const KEY_INSTANCES_COUNT = "InstancesCount";
static const char* const KEY_UNCOMPRESSED_SIZE_MB = "UncompressedSizeMB";
static const char* const KEY_ARCHIVE_SIZE_MB = "ArchiveSizeMB";
static const char* const KEY_UNCOMPRESSED_SIZE = "UncompressedSize";
static const char* const KEY_ARCHIVE_SIZE = "ArchiveSize";
static const char* const KEY_TRANSCODE = "Transcode";

static const int NB_THREAD = 3;


namespace Orthanc
{
  static bool IsZip64Required(uint64_t uncompressedSize,
                              unsigned int countInstances)
  {
    static const uint64_t      SAFETY_MARGIN = 64 * MEGA_BYTES;  // Should be large enough to hold DICOMDIR
    static const unsigned int  FILES_MARGIN = 10;

    /**
     * Determine whether ZIP64 is required. Original ZIP format can
     * store up to 2GB of data (some implementation supporting up to
     * 4GB of data), and up to 65535 files.
     * https://en.wikipedia.org/wiki/Zip_(file_format)#ZIP64
     **/

    const bool isZip64 = (uncompressedSize >= 2 * GIGA_BYTES - SAFETY_MARGIN ||
                          countInstances >= 65535 - FILES_MARGIN);

    LOG(INFO) << "Creating a ZIP file with " << countInstances << " files of size "
              << (uncompressedSize / MEGA_BYTES) << "MB using the "
              << (isZip64 ? "ZIP64" : "ZIP32") << " file format";

    return isZip64;
  }


  class ArchiveJob::ResourceIdentifiers : public boost::noncopyable
  {
  private:
    ResourceType   level_;
    std::string    patient_;
    std::string    study_;
    std::string    series_;
    std::string    instance_;

    static void GoToParent(ServerIndex& index,
                           std::string& current)
    {
      std::string tmp;

      if (index.LookupParent(tmp, current))
      {
        current = tmp;
      }
      else
      {
        throw OrthancException(ErrorCode_UnknownResource);
      }
    }


  public:
    ResourceIdentifiers(ServerIndex& index,
                        const std::string& publicId)
    {
      if (!index.LookupResourceType(level_, publicId))
      {
        throw OrthancException(ErrorCode_UnknownResource);
      }

      std::string current = publicId;;
      switch (level_)  // Do not add "break" below!
      {
        case ResourceType_Instance:
          instance_ = current;
          GoToParent(index, current);
            
        case ResourceType_Series:
          series_ = current;
          GoToParent(index, current);

        case ResourceType_Study:
          study_ = current;
          GoToParent(index, current);

        case ResourceType_Patient:
          patient_ = current;
          break;

        default:
          throw OrthancException(ErrorCode_InternalError);
      }
    }

    ResourceType GetLevel() const
    {
      return level_;
    }

    const std::string& GetIdentifier(ResourceType level) const
    {
      // Some sanity check to ensure enumerations are not altered
      assert(ResourceType_Patient < ResourceType_Study);
      assert(ResourceType_Study < ResourceType_Series);
      assert(ResourceType_Series < ResourceType_Instance);

      if (level > level_)
      {
        throw OrthancException(ErrorCode_InternalError);
      }

      switch (level)
      {
        case ResourceType_Patient:
          return patient_;

        case ResourceType_Study:
          return study_;

        case ResourceType_Series:
          return series_;

        case ResourceType_Instance:
          return instance_;

        default:
          throw OrthancException(ErrorCode_InternalError);
      }
    }
  };


  class ArchiveJob::IArchiveVisitor : public boost::noncopyable
  {
  public:
    virtual ~IArchiveVisitor()
    {
    }

    virtual void Open(ResourceType level,
                      const std::string& publicId) = 0;

    virtual void Close() = 0;

    virtual void AddInstance(const std::string& instanceId,
                             uint64_t uncompressedSize) = 0;
  };


  class ArchiveJob::ArchiveIndex : public boost::noncopyable
  {
  private:
    struct Instance
    {
      std::string  id_;
      uint64_t     uncompressedSize_;

      Instance(const std::string& id,
               uint64_t uncompressedSize) : 
        id_(id),
        uncompressedSize_(uncompressedSize)
      {
      }
    };

    // A "NULL" value for ArchiveIndex indicates a non-expanded node
    typedef std::map<std::string, ArchiveIndex*>   Resources;

    ResourceType         level_;
    Resources            resources_;   // Only at patient/study/series level
    std::list<Instance>  instances_;   // Only at instance level


    void AddResourceToExpand(ServerIndex& index,
                             const std::string& id)
    {
      if (level_ == ResourceType_Instance)
      {
        FileInfo tmp;
        int64_t revision;  // ignored
        if (index.LookupAttachment(tmp, revision, id, FileContentType_Dicom))
        {
          instances_.push_back(Instance(id, tmp.GetUncompressedSize()));
        }
      }
      else
      {
        resources_[id] = NULL;
      }
    }


  public:
    explicit ArchiveIndex(ResourceType level) :
      level_(level)
    {
    }

    ~ArchiveIndex()
    {
      for (Resources::iterator it = resources_.begin();
           it != resources_.end(); ++it)
      {
        delete it->second;
      }
    }


    void Add(ServerIndex& index,
             const ResourceIdentifiers& resource)
    {
      const std::string& id = resource.GetIdentifier(level_);
      Resources::iterator previous = resources_.find(id);

      if (level_ == ResourceType_Instance)
      {
        AddResourceToExpand(index, id);
      }
      else if (resource.GetLevel() == level_)
      {
        // Mark this resource for further expansion
        if (previous != resources_.end())
        {
          delete previous->second;
        }

        resources_[id] = NULL;
      }
      else if (previous == resources_.end())
      {
        // This is the first time we meet this resource
        std::unique_ptr<ArchiveIndex> child(new ArchiveIndex(GetChildResourceType(level_)));
        child->Add(index, resource);
        resources_[id] = child.release();
      }
      else if (previous->second != NULL)
      {
        previous->second->Add(index, resource);
      }
      else
      {
        // Nothing to do: This item is marked for further expansion
      }
    }


    void Expand(ServerIndex& index)
    {
      if (level_ == ResourceType_Instance)
      {
        // Expanding an instance node makes no sense
        return;
      }

      for (Resources::iterator it = resources_.begin();
           it != resources_.end(); ++it)
      {
        if (it->second == NULL)
        {
          // This is resource is marked for expansion
          std::list<std::string> children;
          index.GetChildren(children, it->first);

          std::unique_ptr<ArchiveIndex> child(new ArchiveIndex(GetChildResourceType(level_)));

          for (std::list<std::string>::const_iterator 
                 it2 = children.begin(); it2 != children.end(); ++it2)
          {
            child->AddResourceToExpand(index, *it2);
          }

          it->second = child.release();
        }

        assert(it->second != NULL);
        it->second->Expand(index);
      }        
    }


    void Apply(IArchiveVisitor& visitor) const
    {
      if (level_ == ResourceType_Instance)
      {
        for (std::list<Instance>::const_iterator 
               it = instances_.begin(); it != instances_.end(); ++it)
        {
          visitor.AddInstance(it->id_, it->uncompressedSize_);
        }          
      }
      else
      {
        for (Resources::const_iterator it = resources_.begin();
             it != resources_.end(); ++it)
        {
          assert(it->second != NULL);  // There must have been a call to "Expand()"
          visitor.Open(level_, it->first);
          it->second->Apply(visitor);
          visitor.Close();
        }
      }
    }
  };



  class ArchiveJob::ZipCommands : public boost::noncopyable
  {
  private:
    enum Type
    {
      Type_OpenDirectory,
      Type_CloseDirectory,
      Type_WriteInstance
    };

    class Command : public boost::noncopyable
    {
    private:
      Type          type_;
      std::string   filename_;
      std::string   instanceId_;

    public:
      explicit Command(Type type) :
        type_(type)
      {
        assert(type_ == Type_CloseDirectory);
      }
        
      Command(Type type,
              const std::string& filename) :
        type_(type),
        filename_(filename)
      {
        assert(type_ == Type_OpenDirectory);
      }
        
      Command(Type type,
              const std::string& filename,
              const std::string& instanceId) :
        type_(type),
        filename_(filename),
        instanceId_(instanceId)
      {
        assert(type_ == Type_WriteInstance);
      }

      bool IsWriteInstance(){
        return type_ == Type_WriteInstance;
      }
        
      void Apply(HierarchicalZipWriter& writer,
                 ServerContext& context,
                 DicomDirWriter* dicomDir,
                 const std::string& dicomDirFolder,
                 bool transcode,
                 DicomTransferSyntax transferSyntax,
                 bool reading,
                 std::string& content) const
      {
        switch (type_)
        {
          case Type_OpenDirectory:
            writer.OpenDirectory(filename_.c_str());
            break;

          case Type_CloseDirectory:
            writer.CloseDirectory();
            break;

          case Type_WriteInstance:
          {
            if (reading)
            {
              try
              {
                context.ReadDicom(content, instanceId_);
              }
              catch (OrthancException& e)
              {
                LOG(WARNING) << "An instance was removed after the job was issued: " << instanceId_;
                return;
              }
            }
            else
            {
              //boost::this_thread::sleep(boost::posix_time::milliseconds(300));

              writer.OpenFile(filename_.c_str());

              bool transcodeSuccess = false;

              std::unique_ptr<ParsedDicomFile> parsed;
              
              if (transcode)
              {
                // New in Orthanc 1.7.0
                std::set<DicomTransferSyntax> syntaxes;
                syntaxes.insert(transferSyntax);

                IDicomTranscoder::DicomImage source, transcoded;
                source.SetExternalBuffer(content);

                if (context.Transcode(transcoded, source, syntaxes, true /* allow new SOP instance UID */))
                {
                  writer.Write(transcoded.GetBufferData(), transcoded.GetBufferSize());

                  if (dicomDir != NULL)
                  {
                    std::unique_ptr<ParsedDicomFile> tmp(transcoded.ReleaseAsParsedDicomFile());
                    dicomDir->Add(dicomDirFolder, filename_, *tmp);
                  }
                  
                  transcodeSuccess = true;
                }
                else
                {
                  LOG(INFO) << "Cannot transcode instance " << instanceId_
                            << " to transfer syntax: " << GetTransferSyntaxUid(transferSyntax);
                }
              }

              if (!transcodeSuccess)
              {
                writer.Write(content);

                if (dicomDir != NULL)
                {
                  if (parsed.get() == NULL)
                  {
                    parsed.reset(new ParsedDicomFile(content));
                  }

                  dicomDir->Add(dicomDirFolder, filename_, *parsed);
                }
              }
             
            }        
            
              
            break;
          }

          default:
            throw OrthancException(ErrorCode_InternalError);
        }
      }
    };
      
    std::deque<Command*>  commands_;
    uint64_t              uncompressedSize_;
    unsigned int          instancesCount_;

      
    void ApplyInternal(HierarchicalZipWriter& writer,
                       ServerContext& context,
                       size_t index,
                       DicomDirWriter* dicomDir,
                       const std::string& dicomDirFolder,
                       bool transcode,
                       DicomTransferSyntax transferSyntax,
                       bool reading,
                       std::string& content) const
    {
      if (index >= commands_.size())
      {
        throw OrthancException(ErrorCode_ParameterOutOfRange);
      }

      commands_[index]->Apply(writer, context, dicomDir, dicomDirFolder, transcode, transferSyntax, reading, content);
    }
      
  public:
    ZipCommands() :
      uncompressedSize_(0),
      instancesCount_(0)
    {
    }
      
    ~ZipCommands()
    {
      for (std::deque<Command*>::iterator it = commands_.begin();
           it != commands_.end(); ++it)
      {
        assert(*it != NULL);
        delete *it;
      }
    }

    bool CommandIsWriteInstance(size_t index)
    {
      return commands_[index]->IsWriteInstance();
    }

    size_t GetSize() const
    {
      return commands_.size();
    }

    unsigned int GetInstancesCount() const
    {
      return instancesCount_;
    }

    uint64_t GetUncompressedSize() const
    {
      return uncompressedSize_;
    }

    // "media" flavor (with DICOMDIR)
    void Apply(HierarchicalZipWriter& writer,
               ServerContext& context,
               size_t index,
               DicomDirWriter& dicomDir,
               const std::string& dicomDirFolder,
               bool transcode,
               DicomTransferSyntax transferSyntax,
               bool reading,
               std::string& content) const
    {
      ApplyInternal(writer, context, index, &dicomDir, dicomDirFolder, transcode, transferSyntax, reading, content);
    }

    // "archive" flavor (without DICOMDIR)
    void Apply(HierarchicalZipWriter& writer,
               ServerContext& context,
               size_t index,
               bool transcode,
               DicomTransferSyntax transferSyntax,
               bool reading,
               std::string& content) const
    {
      ApplyInternal(writer, context, index, NULL, "", transcode, transferSyntax, reading, content);
    }
      
    void AddOpenDirectory(const std::string& filename)
    {
      commands_.push_back(new Command(Type_OpenDirectory, filename));
    }

    void AddCloseDirectory()
    {
      commands_.push_back(new Command(Type_CloseDirectory));
    }

    void AddWriteInstance(const std::string& filename,
                          const std::string& instanceId,
                          uint64_t uncompressedSize)
    {
      commands_.push_back(new Command(Type_WriteInstance, filename, instanceId));
      instancesCount_ ++;
      uncompressedSize_ += uncompressedSize;
    }

    bool IsZip64() const
    {
      return IsZip64Required(GetUncompressedSize(), GetInstancesCount());
    }
  };
    
    

  class ArchiveJob::ArchiveIndexVisitor : public IArchiveVisitor
  {
  private:
    ZipCommands&    commands_;
    ServerContext&  context_;
    char            instanceFormat_[24];
    unsigned int    counter_;

    static std::string GetTag(const DicomMap& tags,
                              const DicomTag& tag)
    {
      const DicomValue* v = tags.TestAndGetValue(tag);
      if (v != NULL &&
          !v->IsBinary() &&
          !v->IsNull())
      {
        return v->GetContent();
      }
      else
      {
        return "";
      }
    }

  public:
    ArchiveIndexVisitor(ZipCommands& commands,
                        ServerContext& context) :
      commands_(commands),
      context_(context),
      counter_(0)
    {
      if (commands.GetSize() != 0)
      {
        throw OrthancException(ErrorCode_BadSequenceOfCalls);
      }
        
      snprintf(instanceFormat_, sizeof(instanceFormat_) - 1, "%%08d.dcm");
    }

    virtual void Open(ResourceType level,
                      const std::string& publicId) ORTHANC_OVERRIDE
    {
      std::string path;

      DicomMap tags;
      if (context_.GetIndex().GetMainDicomTags(tags, publicId, level, level))
      {
        switch (level)
        {
          case ResourceType_Patient:
            path = GetTag(tags, DICOM_TAG_PATIENT_ID) + " " + GetTag(tags, DICOM_TAG_PATIENT_NAME);
            break;

          case ResourceType_Study:
            path = GetTag(tags, DICOM_TAG_ACCESSION_NUMBER) + " " + GetTag(tags, DICOM_TAG_STUDY_DESCRIPTION);
            break;

          case ResourceType_Series:
          {
            std::string modality = GetTag(tags, DICOM_TAG_MODALITY);
            path = modality + " " + GetTag(tags, DICOM_TAG_SERIES_DESCRIPTION);

            if (modality.size() == 0)
            {
              snprintf(instanceFormat_, sizeof(instanceFormat_) - 1, "%%08d.dcm");
            }
            else if (modality.size() == 1)
            {
              snprintf(instanceFormat_, sizeof(instanceFormat_) - 1, "%c%%07d.dcm", 
                       toupper(modality[0]));
            }
            else if (modality.size() >= 2)
            {
              snprintf(instanceFormat_, sizeof(instanceFormat_) - 1, "%c%c%%06d.dcm", 
                       toupper(modality[0]), toupper(modality[1]));
            }

            counter_ = 0;

            break;
          }

          default:
            throw OrthancException(ErrorCode_InternalError);
        }
      }

      path = Toolbox::StripSpaces(Toolbox::ConvertToAscii(path));

      if (path.empty())
      {
        path = std::string("Unknown ") + EnumerationToString(level);
      }

      commands_.AddOpenDirectory(path.c_str());
    }

    virtual void Close() ORTHANC_OVERRIDE
    {
      commands_.AddCloseDirectory();
    }

    virtual void AddInstance(const std::string& instanceId,
                             uint64_t uncompressedSize) ORTHANC_OVERRIDE
    {
      char filename[24];
      snprintf(filename, sizeof(filename) - 1, instanceFormat_, counter_);
      counter_ ++;

      commands_.AddWriteInstance(filename, instanceId, uncompressedSize);
    }
  };

    
  class ArchiveJob::MediaIndexVisitor : public IArchiveVisitor
  {
  private:
    ZipCommands&    commands_;
    unsigned int    counter_;

  public:
    explicit MediaIndexVisitor(ZipCommands& commands) :
      commands_(commands),
      counter_(0)
    {
    }

    virtual void Open(ResourceType level,
                      const std::string& publicId) ORTHANC_OVERRIDE
    {
    }

    virtual void Close() ORTHANC_OVERRIDE
    {
    }

    virtual void AddInstance(const std::string& instanceId,
                             uint64_t uncompressedSize) ORTHANC_OVERRIDE
    {
      // "DICOM restricts the filenames on DICOM media to 8
      // characters (some systems wrongly use 8.3, but this does not
      // conform to the standard)."
      std::string filename = "IM" + boost::lexical_cast<std::string>(counter_);
      commands_.AddWriteInstance(filename, instanceId, uncompressedSize);

      counter_ ++;
    }
  };


  class ArchiveJob::ZipWriterIterator : public boost::noncopyable
  {
  private:
    ServerContext&                          context_;
    ZipCommands                             commands_;
    std::unique_ptr<HierarchicalZipWriter>  zip_;
    std::unique_ptr<DicomDirWriter>         dicomDir_;
    bool                                    isMedia_;
    bool                                    isStream_;

  public:
    ZipWriterIterator(ServerContext& context,
                      ArchiveIndex& archive,
                      bool isMedia,
                      bool enableExtendedSopClass) :
      context_(context),
      isMedia_(isMedia),
      isStream_(false)
    {
      if (isMedia)
      {
        MediaIndexVisitor visitor(commands_);
        archive.Expand(context.GetIndex());

        commands_.AddOpenDirectory(MEDIA_IMAGES_FOLDER);        
        archive.Apply(visitor);
        commands_.AddCloseDirectory();

        dicomDir_.reset(new DicomDirWriter);
        dicomDir_->EnableExtendedSopClass(enableExtendedSopClass);
      }
      else
      {
        ArchiveIndexVisitor visitor(commands_, context);
        archive.Expand(context.GetIndex());
        archive.Apply(visitor);
      }
    }

    bool CurrentCommandIsWriteInstance(size_t index)
    {
      return commands_.CommandIsWriteInstance(index);
    }

    void SetOutputFile(const std::string& path)
    {
      if (zip_.get() == NULL)
      {
        zip_.reset(new HierarchicalZipWriter(path.c_str()));
        zip_->SetZip64(commands_.IsZip64());
        isStream_ = false;
      }
      else
      {
        throw OrthancException(ErrorCode_BadSequenceOfCalls);
      }
    }

    void AcquireOutputStream(ZipWriter::IOutputStream* output)
    {
      std::unique_ptr<ZipWriter::IOutputStream> protection(output);

      if (zip_.get() == NULL)
      {
        zip_.reset(new HierarchicalZipWriter(protection.release(), commands_.IsZip64()));
        isStream_ = true;
      }
      else
      {
        throw OrthancException(ErrorCode_BadSequenceOfCalls);
      }
    }

    void CancelStream()
    {
      if (zip_.get() == NULL)
      {
        throw OrthancException(ErrorCode_BadSequenceOfCalls);
      }
      else if (isStream_)
      {
        zip_->CancelStream();
      }
    }

    void Close()
    {
      if (zip_.get() == NULL)
      {
        throw OrthancException(ErrorCode_BadSequenceOfCalls);
      }
      else
      {
        zip_->Close();
      }
    }

    uint64_t GetArchiveSize() const
    {
      if (zip_.get() == NULL)
      {
        throw OrthancException(ErrorCode_BadSequenceOfCalls);
      }
      else
      {
        return zip_->GetArchiveSize();
      }
    }

    size_t GetStepsCount() const
    {
      return commands_.GetSize() + 1;
    }

    void RunStep(size_t index,
                 bool transcode,
                 DicomTransferSyntax transferSyntax,
                 bool reading,
                 std::string& content)
    {
      if (index > commands_.GetSize())
      {
        throw OrthancException(ErrorCode_ParameterOutOfRange);
      }
      else if (zip_.get() == NULL)
      {
        throw OrthancException(ErrorCode_BadSequenceOfCalls);
      }
      else if (index != commands_.GetSize())
      {
        if (isMedia_)
        {
          assert(dicomDir_.get() != NULL);
          commands_.Apply(*zip_, context_, index, *dicomDir_,
                          MEDIA_IMAGES_FOLDER, transcode, transferSyntax, reading, content);
        }
        else
        {
          assert(dicomDir_.get() == NULL);
          commands_.Apply(*zip_, context_, index, transcode, transferSyntax, reading, content);
        }
      }
    }

    void LastStep()
    {
      if (isMedia_)
      {
        assert(dicomDir_.get() != NULL);
        std::string s;
        dicomDir_->Encode(s);

        zip_->OpenFile("DICOMDIR");
        zip_->Write(s);
      }
    }

    unsigned int GetInstancesCount() const
    {
      return commands_.GetInstancesCount();
    }

    uint64_t GetUncompressedSize() const
    {
      return commands_.GetUncompressedSize();
    }
  };
  
  struct Threads
  {
    std::thread        thread;
    bool               boolRead;
    std::string        content;
    std::atomic<bool>  threadFinished;
    int                threadCurrentStep;
  };
  
  Threads threads_[NB_THREAD];
  int threadsActive_;

  ArchiveJob::ArchiveJob(ServerContext& context,
                         bool isMedia,
                         bool enableExtendedSopClass) :
    context_(context),
    archive_(new ArchiveIndex(ResourceType_Patient)),  // root
    isMedia_(isMedia),
    enableExtendedSopClass_(enableExtendedSopClass),
    currentStep_(0),
    instancesCount_(0),
    uncompressedSize_(0),
    archiveSize_(0),
    transcode_(false),
    transferSyntax_(DicomTransferSyntax_LittleEndianImplicit)
  {
    threadsActive_ = 0;
    for (int i = 0; i < NB_THREAD; i++)
    {
      threads_[i].threadFinished = true;
    }
  }

  
  ArchiveJob::~ArchiveJob()
  {
    if (!mediaArchiveId_.empty())
    {
      context_.GetMediaArchive().Remove(mediaArchiveId_);
    }
  }


  void ArchiveJob::AcquireSynchronousTarget(ZipWriter::IOutputStream* target)
  {
    std::unique_ptr<ZipWriter::IOutputStream> protection(target);
    
    if (target == NULL)
    {
      throw OrthancException(ErrorCode_NullPointer);
    }
    else if (writer_.get() != NULL ||  // Already started
             synchronousTarget_.get() != NULL ||
             asynchronousTarget_.get() != NULL)
    {
      throw OrthancException(ErrorCode_BadSequenceOfCalls);
    }
    else
    {
      synchronousTarget_.reset(protection.release());
    }
  }


  void ArchiveJob::SetDescription(const std::string& description)
  {
    if (writer_.get() != NULL)   // Already started
    {
      throw OrthancException(ErrorCode_BadSequenceOfCalls);
    }
    else
    {
      description_ = description;
    }
  }

  
  void ArchiveJob::AddResource(const std::string& publicId)
  {
    if (writer_.get() != NULL)   // Already started
    {
      throw OrthancException(ErrorCode_BadSequenceOfCalls);
    }
    else
    {
      ResourceIdentifiers resource(context_.GetIndex(), publicId);
      archive_->Add(context_.GetIndex(), resource);
    }
  }


  void ArchiveJob::SetTranscode(DicomTransferSyntax transferSyntax)
  {
    if (writer_.get() != NULL)   // Already started
    {
      throw OrthancException(ErrorCode_BadSequenceOfCalls);
    }
    else
    {
      transcode_ = true;
      transferSyntax_ = transferSyntax;
    }
  }

  
  void ArchiveJob::Reset()
  {
    throw OrthancException(ErrorCode_BadSequenceOfCalls,
                           "Cannot resubmit the creation of an archive");
  }

  
  void ArchiveJob::Start()
  {
    if (writer_.get() != NULL)
    {
      throw OrthancException(ErrorCode_BadSequenceOfCalls);
    }
    else
    {
      if (synchronousTarget_.get() == NULL)
      {
        if (asynchronousTarget_.get() != NULL)
        {
          // It is up to this method to create the asynchronous target
          throw OrthancException(ErrorCode_InternalError);
        }
        else
        {
          OrthancConfiguration::ReaderLock lock;
          asynchronousTarget_.reset(lock.GetConfiguration().CreateTemporaryFile());
          
          assert(asynchronousTarget_.get() != NULL);
          asynchronousTarget_->Touch();  // Make sure we can write to the temporary file
          
          writer_.reset(new ZipWriterIterator(context_, *archive_, isMedia_, enableExtendedSopClass_));
          writer_->SetOutputFile(asynchronousTarget_->GetPath());
        }
      }
      else
      {
        assert(synchronousTarget_.get() != NULL);
    
        writer_.reset(new ZipWriterIterator(context_, *archive_, isMedia_, enableExtendedSopClass_));
        writer_->AcquireOutputStream(synchronousTarget_.release());
      }

      instancesCount_ = writer_->GetInstancesCount();
      uncompressedSize_ = writer_->GetUncompressedSize();
    }
  }



  namespace
  {
    class DynamicTemporaryFile : public IDynamicObject
    {
    private:
      std::unique_ptr<TemporaryFile>   file_;

    public:
      explicit DynamicTemporaryFile(TemporaryFile* f) : file_(f)
      {
        if (f == NULL)
        {
          throw OrthancException(ErrorCode_NullPointer);
        }
      }

      const TemporaryFile& GetFile() const
      {
        assert(file_.get() != NULL);
        return *file_;
      }
    };
  }
  

  void ArchiveJob::FinalizeTarget()
  {
    if (writer_.get() != NULL)
    {
      writer_->Close();  // Flush all the results
      archiveSize_ = writer_->GetArchiveSize();
      writer_.reset();
    }

    if (asynchronousTarget_.get() != NULL)
    {
      // Asynchronous behavior: Move the resulting file into the media archive
      mediaArchiveId_ = context_.GetMediaArchive().Add(
        new DynamicTemporaryFile(asynchronousTarget_.release()));
    }
  }

  //method run by thread, read only DICOM
  void ArchiveJob::RunStepReadInstance(const int numThread)
  {
    writer_->RunStep(threads_[numThread].threadCurrentStep, transcode_, transferSyntax_, threads_[numThread].boolRead, threads_[numThread].content);
    threads_[numThread].threadFinished = true;
  }
  

  JobStepResult ArchiveJob::Step(const std::string& jobId)
  {
    assert(writer_.get() != NULL);

    if (writer_->GetStepsCount() == 0)
    {
      FinalizeTarget();
      return JobStepResult::Success();
    }
    else
    {      
      try
      {
        if (currentStep_ != writer_->GetStepsCount() - 1)
        {
          //If it is a Command to open / close a directory, we wait for the end of the active thread and we launch the step
          if (!writer_->CurrentCommandIsWriteInstance(currentStep_))
          {
            for (int i = 0; i < threadsActive_; i++)
            {
              bool err = EndThreadIndexOf(i);
              if(!err)
              {
                std::cout << "Error reading / writing the dicom via the thread\n";
              }
            }
            writer_->RunStep(currentStep_, transcode_, transferSyntax_, threads_[threadsActive_].boolRead, threads_[threadsActive_].content);
            threadsActive_ = 0;
          }
          else
          {
            //if the thread queue is not full we complete it
            if (threadsActive_ < NB_THREAD)
            {
              threads_[threadsActive_].threadFinished = false;
              threads_[threadsActive_].boolRead = true;
              threads_[threadsActive_].threadCurrentStep = currentStep_;
              threads_[threadsActive_].thread = std::thread(&ArchiveJob::RunStepReadInstance, this, threadsActive_);
              threadsActive_++;
            }
            else
            {
              //if not, we loop until we find a terminated thread, we write the instance and start a new one
              int threadFound = false;
              int i = 0;
              while (!threadFound)
              {
                if (threads_[i].threadFinished)
                {
                  bool err = EndThreadIndexOf(i);
                  if(!err)
                  {
                    std::cout << "Error reading / writing the dicom via the thread\n";
                  }
                  // launch dicom reading thread
                  threads_[i].threadFinished = false;
                  threads_[i].threadCurrentStep = currentStep_;
                  threads_[i].thread = std::thread(&ArchiveJob::RunStepReadInstance, this, i);
                  threadFound = true;
                }
                i++;
                i = i%3;
              }
            }
          }
        }
      }
      catch (Orthanc::OrthancException& e)
      {
        LOG(ERROR) << "Error while creating an archive: " << e.What();
        writer_->CancelStream();
        throw;
      }
      
      //If this is the last step, we wait for all the threads to end
      if (currentStep_ == writer_->GetStepsCount() - 1)
      {
        for (int i = 0; i < threadsActive_; i++)
        {
          bool err = EndThreadIndexOf(i);
          if(!err)
          {
            std::cout << "Error reading / writing the dicom via the thread\n";
          }
        }   
        //Run last step ADD DICOMDIR
        writer_->LastStep();
        threadsActive_ = 0;
      }

      currentStep_ ++;

      if (currentStep_ == writer_->GetStepsCount())
      {
        FinalizeTarget();
        return JobStepResult::Success();
      }
      else
      {
        archiveSize_ = writer_->GetArchiveSize();
        return JobStepResult::Continue();
      }
    }
  }

  bool ArchiveJob::EndThreadIndexOf(const int threadNumber)
  {
    //end of dicom reading
    threads_[threadNumber].thread.join();
    //writing dicom
    threads_[threadNumber].boolRead = false;
    writer_->RunStep(threads_[threadNumber].threadCurrentStep, transcode_, transferSyntax_, threads_[threadNumber].boolRead, threads_[threadNumber].content);
    threads_[threadNumber].boolRead = true;
    threads_[threadNumber].content = "";
    return true;
  }


  void ArchiveJob::Stop(JobStopReason reason)
  {
    /**
     * New in Orthanc 1.9.3: Remove the temporary file associated with
     * the job as soon as its job gets canceled (especially visible in
     * asynchronous mode).
     **/
    if (reason == JobStopReason_Canceled ||
        reason == JobStopReason_Failure ||
        reason == JobStopReason_Retry)
    {
      writer_->CancelStream();
      
      // First delete the writer, as it holds a reference to "(a)synchronousTarget_", cf. (*)
      writer_.reset();
      
      synchronousTarget_.reset();
      asynchronousTarget_.reset();
    }
  }


  float ArchiveJob::GetProgress()
  {
    if (writer_.get() == NULL ||
        writer_->GetStepsCount() == 0)
    {
      return 1;
    }
    else
    {
      return (static_cast<float>(currentStep_) /
              static_cast<float>(writer_->GetStepsCount() - 1));
    }
  }

    
  void ArchiveJob::GetJobType(std::string& target)
  {
    if (isMedia_)
    {
      target = "Media";
    }
    else
    {
      target = "Archive";
    }
  }


  void ArchiveJob::GetPublicContent(Json::Value& value)
  {
    value = Json::objectValue;
    value[KEY_DESCRIPTION] = description_;
    value[KEY_INSTANCES_COUNT] = instancesCount_;
    value[KEY_UNCOMPRESSED_SIZE_MB] =
      static_cast<unsigned int>(uncompressedSize_ / MEGA_BYTES);
    value[KEY_ARCHIVE_SIZE_MB] =
      static_cast<unsigned int>(archiveSize_ / MEGA_BYTES);

    // New in Orthanc 1.9.4
    value[KEY_ARCHIVE_SIZE] = boost::lexical_cast<std::string>(archiveSize_);
    value[KEY_UNCOMPRESSED_SIZE] = boost::lexical_cast<std::string>(uncompressedSize_);

    if (transcode_)
    {
      value[KEY_TRANSCODE] = GetTransferSyntaxUid(transferSyntax_);
    }
  }


  bool ArchiveJob::GetOutput(std::string& output,
                             MimeType& mime,
                             const std::string& key)
  {   
    if (key == "archive" &&
        !mediaArchiveId_.empty())
    {
      SharedArchive::Accessor accessor(context_.GetMediaArchive(), mediaArchiveId_);

      if (accessor.IsValid())
      {
        const DynamicTemporaryFile& f = dynamic_cast<DynamicTemporaryFile&>(accessor.GetItem());
        f.GetFile().Read(output);
        mime = MimeType_Zip;
        return true;
      }
      else
      {
        return false;
      }
    }    
    else
    {
      return false;
    }
  }
}
