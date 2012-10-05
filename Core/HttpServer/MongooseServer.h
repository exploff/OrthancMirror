/**
 * Orthanc - A Lightweight, RESTful DICOM Store
 * Copyright (C) 2012 Medical Physics Department, CHU of Liege,
 * Belgium
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 **/


#pragma once

#include "HttpHandler.h"

#include <list>
#include <map>
#include <stdint.h>
#include <boost/shared_ptr.hpp>

namespace Orthanc
{
  class ChunkStore;

  class MongooseServer
  {
  private:
    // http://stackoverflow.com/questions/311166/stdauto-ptr-or-boostshared-ptr-for-pimpl-idiom
    struct PImpl;
    boost::shared_ptr<PImpl> pimpl_;

    typedef std::list<HttpHandler*> Handlers;
    Handlers handlers_;

    typedef std::set<std::string> RegisteredUsers;
    RegisteredUsers registeredUsers_;

    bool remoteAllowed_;
    bool authentication_;
    bool ssl_;
    std::string certificate_;
    uint16_t port_;
  
    bool IsRunning() const;

  public:
    MongooseServer();

    ~MongooseServer();

    void SetPortNumber(uint16_t port);

    uint16_t GetPortNumber() const
    {
      return port_;
    }

    void Start();

    void Stop();

    void ClearUsers();

    void RegisterUser(const char* username,
                      const char* password);

    void RegisterHandler(HttpHandler* handler);  // This takes the ownership

    bool IsAuthenticationEnabled() const
    {
      return authentication_;
    }

    void SetAuthenticationEnabled(bool enabled);

    bool IsSslEnabled() const
    {
      return ssl_;
    }

    void SetSslEnabled(bool enabled);

    const std::string& GetSslCertificate() const
    {
      return certificate_;
    }

    void SetSslCertificate(const char* path);

    bool IsRemoteAccessAllowed() const
    {
      return remoteAllowed_;
    }

    void SetRemoteAccessAllowed(bool allowed);

    void ClearHandlers();

    // Can return NULL if no handler is associated to this URI
    HttpHandler* FindHandler(const UriComponents& forUri) const;

    ChunkStore& GetChunkStore();

    bool IsValidBasicHttpAuthentication(const std::string& basic) const;
  };
}
