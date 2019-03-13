/**
 * @file WebSocketServer.cpp
 * @author paul
 * @date 10.03.19
 * @brief WebSocketServer @TODO
 */

#include <iostream>
#include "WebSocketServer.hpp"

namespace network {
    std::map<lws_context*, WebSocketServer*> WebSocketServer::instances;

    WebSocketServer::WebSocketServer(uint16_t port, const std::string &protocolName) :
        finished{false},
        protocolName{protocolName},
        context{nullptr, lws_context_destroy},
        protocols{
            {
                this->protocolName.c_str(),
                &WebSocketServer::globalHandler,
                sizeof(int),
                4096,
                0,
                nullptr,
                0
            },
            {
                nullptr, nullptr, 0, 0, 0, nullptr, 0 // Quasi null terminator
            }
        },
        callList{std::make_shared<std::pair<std::list<std::function<void()>>,std::mutex>>()},
        connectionUidCount{0} {

        lws_context_creation_info contextCreationInfo{};
        contextCreationInfo.port = port;
        contextCreationInfo.protocols = this->protocols.data();
        contextCreationInfo.gid = -1;
        contextCreationInfo.uid = -1;

        this->context = decltype(this->context){lws_create_context(&contextCreationInfo),
                                                     lws_context_destroy};

        if (!this->context) {
            throw std::runtime_error("Could not initialize websocket");
        }
        instances.insert({this->context.get(), this});

        this->workerThread = std::thread{&WebSocketServer::run, this};
    }

    void WebSocketServer::run() {
        while (!finished) {
            lws_service(this->context.get(), 50);

            std::lock_guard<std::mutex> lock{this->callList->second};
            for (const auto &call : this->callList->first) {
                call();
            }
            this->callList->first.clear();
        }
    }

    void WebSocketServer::sendImpl(std::string text, lws *wsi) {
        lws_write(wsi, reinterpret_cast<unsigned char*>(text.data()), text.length(), LWS_WRITE_TEXT);
    }

    void WebSocketServer::broadcast(std::string text) {
        for (const auto &connection : this->connections) {
            connection.second->send(text);
        }
    }

    int WebSocketServer::handler(lws *websocket, lws_callback_reasons reasons, int *userData, std::string text) {
        switch (reasons) {
            case LWS_CALLBACK_ESTABLISHED: {
                *userData = ++connectionUidCount;
                auto connection = std::make_shared<Connection>(Connection{websocket, this->callList});
                connections.emplace(std::make_pair(*userData, connection));
                this->connectionListener(connection);
                std::cout << "Established (" << *userData << ")" << std::endl;
                break;
            }
            case LWS_CALLBACK_CLOSED: {
                std::cout << "Closed (" << *userData << ")" << std::endl;
                auto it = connections.find(*userData);
                if (it != connections.end()) {
                    it->second->receiveListener(text);
                    this->closeListener(it->second);
                    it->second->socket = nullptr;
                    connections.erase(*userData);
                }
                break;
            }
            case LWS_CALLBACK_RECEIVE: {
                std::cout << "Recv (" << *userData << "): " << text << std::endl;
                auto it = connections.find(*userData);
                if (it != connections.end()) {
                    it->second->receiveListener(text);
                }
                break;
            }
            default:
                break;
        }

        return 0;
    }

    WebSocketServer::~WebSocketServer() {
        finished = true;
        workerThread.join();
        instances.erase(this->context.get());
    }

    int WebSocketServer::globalHandler(lws *websocket, lws_callback_reasons reasons, void *userData, void *data,
                                       size_t len) {
        auto *id = static_cast<int*>(userData);
        std::string text{static_cast<char *>(data), len};
        auto *ctx = lws_get_context(websocket);
        auto instance = instances.find(ctx);
        if (instance != instances.end()) {
            return instance->second->handler(websocket, reasons, id, text);
        }
        return 0;
    }
}
