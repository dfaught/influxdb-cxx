// MIT License
//
// Copyright (c) 2020-2023 offa
// Copyright (c) 2019 Adam Wegrzynek
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

///
/// \author Adam Wegrzynek <adam.wegrzynek@cern.ch>
///

#include <thread>
#include <mutex>
#include <chrono>
#include "HTTP.h"
#include "InfluxDBException.h"

using namespace std::chrono_literals;

namespace influxdb::transports
{
    namespace
    {
        void checkResponse(const cpr::Response& resp)
        {
            if (resp.error)
            {
                // TODO: message may be empty here
                throw InfluxDBException{"Request error: (" + std::to_string(static_cast<int>(resp.error.code)) + ") " + resp.error.message};
            }
            if (!cpr::status::is_success(resp.status_code))
            {
                throw InfluxDBException{"Request failed: (" + std::to_string(resp.status_code) + ") " + resp.reason};
            }
        }

        std::string parseUrl(const std::string& url)
        {
            const auto questionMarkPosition = url.find('?');

            if (questionMarkPosition == std::string::npos)
            {
                return url;
            }
            if (url.at(questionMarkPosition - 1) == '/')
            {
                return url.substr(0, questionMarkPosition - 1);
            }
            return url.substr(0, questionMarkPosition);
        }

        std::string parseDatabaseName(const std::string& url)
        {
            const auto dbParameterPosition = url.find("?db=");

            if (dbParameterPosition == std::string::npos)
            {
                throw InfluxDBException{"No Database specified"};
            }
            return url.substr(dbParameterPosition + 4);
        }
    }

    std::mutex asyncMtx;

    HTTP::HTTP(const std::string& url)
        : endpointUrl(parseUrl(url)), databaseName(parseDatabaseName(url))
    {
        session = std::make_shared<cpr::Session>();
        session->SetTimeout(cpr::Timeout{std::chrono::seconds{10}});
        session->SetConnectTimeout(cpr::ConnectTimeout{std::chrono::seconds{10}});
        session->SetVerifySsl( cpr::VerifySsl( false ) );
    }

    HTTP::HTTP(const std::string& url, bool enableAsync)
        : HTTP(url)
    {
        processAsync.store( enableAsync );
        asyncResultHandler = std::thread([&](){ this->handleAsyncResult(); });
    }

    HTTP::~HTTP()
    {
        if(processAsync.load())
        {
            processAsync.store(false);
            asyncResultHandler.join();
        }
    }

    std::string HTTP::query(const std::string& query)
    {
        session->SetUrl(cpr::Url{endpointUrl + "/query"});
        session->SetParameters(cpr::Parameters{{"db", databaseName}, {"q", query}});

        const auto response = session->Get();
        checkResponse(response);

        return response.text;
    }

    void HTTP::setBasicAuthentication(const std::string& user, const std::string& pass)
    {
        session->SetAuth(cpr::Authentication{user, pass, cpr::AuthMode::BASIC});
    }

    void HTTP::send(std::string&& lineprotocol)
    {
        session->SetUrl(cpr::Url{endpointUrl + "/write"});
        session->SetHeader(cpr::Header{{"Content-Type", "application/json"}});
        session->SetParameters(cpr::Parameters{{"db", databaseName}});
        session->SetBody(cpr::Body{lineprotocol});

        const auto response = session->Post();
        checkResponse(response);
    }

    void HTTP::sendAsync( std::string&& lineprotocol )
    {
        session->SetUrl(cpr::Url{endpointUrl + "/write"});
        session->SetHeader(cpr::Header{{"Content-Type", "application/json"}});
        session->SetParameters(cpr::Parameters{{"db", databaseName}});
        session->SetBody(cpr::Body{lineprotocol});

        std::lock_guard<std::mutex> responseGuard(asyncMtx);
        //NOTE The call to PostAsync returns a cpr::AsyncWrapper.  This call must be in the push in to the queue since
        //cpr::AsyncWrapper explicitly deletes its copy constructor.
        respQueue.push_back( session->PostAsync() );
    }

    void HTTP::handleAsyncResult()
    {
        while( processAsync.load() )
        {
            std::this_thread::sleep_for(100ms);

            std::lock_guard<std::mutex> responseGuard(asyncMtx);
            while( respQueue.size() > 0 )
            {
                respQueue.front().wait();
                try
                {
                    checkResponse( respQueue.front().get() );
                }
                catch(const InfluxDBException& dbe)
                {
                   // TODO Logging callback?  Error list?  Out to stdout?
                }

                respQueue.pop_front();
            }

        }

    }

    void HTTP::setProxy(const Proxy& proxy)
    {
        session->SetProxies(cpr::Proxies{{"http", proxy.getProxy()}, {"https", proxy.getProxy()}});

        if (const auto& auth = proxy.getAuthentication(); auth.has_value())
        {
            session->SetProxyAuth(cpr::ProxyAuthentication{{"http", cpr::EncodedAuthentication{auth->user, auth->password}},
                                                          {"https", cpr::EncodedAuthentication{auth->user, auth->password}}});
        }
    }

    std::string HTTP::execute(const std::string& cmd)
    {
        session->SetUrl(cpr::Url{endpointUrl + "/query"});
        session->SetParameters(cpr::Parameters{{"db", databaseName}, {"q", cmd}});

        const auto response = session->Get();
        checkResponse(response);

        return response.text;
    }

    void HTTP::createDatabase()
    {
        session->SetUrl(cpr::Url{endpointUrl + "/query"});
        session->SetParameters(cpr::Parameters{{"q", "CREATE DATABASE " + databaseName}});

        const auto response = session->Post();
        checkResponse(response);
    }

} // namespace influxdb
