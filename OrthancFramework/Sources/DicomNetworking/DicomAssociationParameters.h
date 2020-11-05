/**
 * Orthanc - A Lightweight, RESTful DICOM Store
 * Copyright (C) 2012-2016 Sebastien Jodogne, Medical Physics
 * Department, University Hospital of Liege, Belgium
 * Copyright (C) 2017-2020 Osimis S.A., Belgium
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program. If not, see
 * <http://www.gnu.org/licenses/>.
 **/


#pragma once

#include "RemoteModalityParameters.h"

#include <json/value.h>

class OFCondition;  // From DCMTK

namespace Orthanc
{
  class ORTHANC_PUBLIC DicomAssociationParameters
  {
  private:
    std::string               localAet_;
    RemoteModalityParameters  remote_;
    uint32_t                  timeout_;

    static void CheckHost(const std::string& host);

  public:
    DicomAssociationParameters();
    
    DicomAssociationParameters(const std::string& localAet,
                               const RemoteModalityParameters& remote);
    
    const std::string& GetLocalApplicationEntityTitle() const;

    void SetLocalApplicationEntityTitle(const std::string& aet);

    const RemoteModalityParameters& GetRemoteModality() const;

    void SetRemoteModality(const RemoteModalityParameters& parameters);
    
    void SetRemoteApplicationEntityTitle(const std::string& aet);

    void SetRemoteHost(const std::string& host);

    void SetRemotePort(uint16_t port);

    void SetRemoteManufacturer(ModalityManufacturer manufacturer);

    bool IsEqual(const DicomAssociationParameters& other) const;

    // Setting it to "0" disables the timeout (infinite wait)
    void SetTimeout(uint32_t seconds);

    uint32_t GetTimeout() const;

    bool HasTimeout() const;

    void SerializeJob(Json::Value& target) const;
    
    static DicomAssociationParameters UnserializeJob(const Json::Value& serialized);
    
    static void SetDefaultTimeout(uint32_t seconds);

    static uint32_t GetDefaultTimeout();
  };
}
