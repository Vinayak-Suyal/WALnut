// =============================================================================
// WALnut - REST API Server  (Phase 5 - Suhavi Jugran)
// src/server/server.cpp
//
// Single-process HTTP server using cpp-httplib (header-only, MIT).
// Serves the React/Vite dashboard from ./dashboard/dist/ and exposes all
// API endpoints the frontend calls.
//
// Usage:  walnut_server [data_dir] [port]   (defaults: ./data  8080)
// =============================================================================

#define CPPHTTPLIB_OPENSSL_SUPPORT 0
#include "walnut/httplib.h"

#include "walnut/storage/storage_manager.hpp"
#include "walnut/storage/file_manager.hpp"
#include "walnut/storage/metadata.hpp"
#include "walnut/storage/page.hpp"
#include "walnut/wal/log_manager.hpp"
#include "walnut/wal/log_record.hpp"
#include "walnut/wal/recovery_manager.hpp"
#include "walnut/txn/concurrent_storage.hpp"
#include "walnut/txn/lock_manager.hpp"
#include "walnut/txn/transaction_manager.hpp"
#include "walnut/txn/deadlock_detector.hpp"
#include "walnut/common/types.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#else
#  include <arpa/inet.h>
#  include <ifaddrs.h>
#  include <netinet/in.h>
#  include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace walnut;

// ---------------------------------------------------------------------------
// Minimal JSON helpers (no third-party JSON lib)
// ---------------------------------------------------------------------------
static std::string jStr(const std::string& s) {
    std::string o; o.reserve(s.size()+2); o+='"';
    for (char c:s){
        if(c=='"')o+="\\\""; else if(c=='\\')o+="\\\\";
        else if(c=='\n')o+="\\n"; else if(c=='\r')o+="\\r";
        else o+=c;
    }
    return o+='"';
}

static std::string formatImageUrl(const std::string& u) {
    if (u.empty()) return "";
    if (u.rfind("http://", 0) == 0 || u.rfind("https://", 0) == 0 || u.rfind("/uploads/", 0) == 0) {
        return u;
    }
    return "/uploads/" + u;
}

static std::string itemJson(const Item& it) {
    std::ostringstream o;
    o<<'{'<<"\"item_id\":"<<jStr(it.item_id)<<','
          <<"\"host_id\":"<<jStr(it.host_id)<<','
          <<"\"title\":"<<jStr(it.title)<<','
          <<"\"category\":"<<jStr(it.category)<<','
          <<"\"condition\":"<<jStr(it.condition)<<','
          <<"\"starting_price\":"<<it.starting_price<<','
          <<"\"status\":"<<jStr(it.status)<<','
          <<"\"current_bid\":"<<it.current_bid<<','
          <<"\"current_winner\":"<<jStr(it.current_winner)<<','
          <<"\"end_time_ms\":"<<it.end_time_ms<<','
          <<"\"top_bid_count\":"<<it.top_bid_count<<','
          <<"\"total_bid_count\":"<<it.total_bid_count<<','
          <<"\"image_urls\":["<<jStr(formatImageUrl(it.image_urls[0]))<<','
                              <<jStr(formatImageUrl(it.image_urls[1]))<<','
                              <<jStr(formatImageUrl(it.image_urls[2]))
          <<"],\"top_bids\":[";
    for(uint32_t i=0;i<it.top_bid_count&&i<3;++i){
        if(i)o<<',';
        o<<"{"<<"\"bid_id\":"<<it.top_bids[i].bid_id
         <<",\"user_id\":"<<jStr(it.top_bids[i].user_id)
         <<",\"amount\":"<<it.top_bids[i].amount
         <<",\"timestamp_ms\":"<<it.top_bids[i].timestamp_ms<<'}';
    }
    o<<"]}";
    return o.str();
}
static std::string userJson(const User& u){
    std::ostringstream o;
    o<<"{\"user_id\":"<<jStr(u.user_id)
     <<",\"username\":"<<jStr(u.username)
     <<",\"email\":"<<jStr(u.email)
     <<",\"phone\":"<<jStr(u.phone)
     <<",\"role\":"<<jStr(u.role)<<'}';
    return o.str();
}

// ---------------------------------------------------------------------------
// In-memory session store  (token -> user_id)  — ephemeral, cleared on restart
// ---------------------------------------------------------------------------
static std::unordered_map<std::string, std::string> g_sessions;
static std::mutex g_sessions_mu;

static std::string genToken() {
    static std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<uint64_t> dist;
    char buf[32];
    snprintf(buf, sizeof(buf), "%016llx%016llx",
             (unsigned long long)dist(rng), (unsigned long long)dist(rng));
    return buf;
}

static std::string sessionForUser(const std::string& uid) {
    std::lock_guard<std::mutex> lk(g_sessions_mu);
    // Reuse existing token if any
    for (auto& [tok, id] : g_sessions) {
        if (id == uid) return tok;
    }
    std::string tok = genToken();
    g_sessions[tok] = uid;
    return tok;
}

static std::string tokenUserId(const std::string& tok) {
    std::lock_guard<std::mutex> lk(g_sessions_mu);
    auto it = g_sessions.find(tok);
    return it != g_sessions.end() ? it->second : "";
}
static std::string bidJson(const Bid& b){
    std::ostringstream o;
    o<<"{\"bid_id\":"<<b.bid_id<<",\"item_id\":"<<jStr(b.item_id)<<",\"user_id\":"<<jStr(b.user_id)
     <<",\"amount\":"<<b.amount<<",\"bid_time\":"<<jStr(b.bid_time)<<'}';
    return o.str();
}

static std::string jField(const std::string& body, const std::string& key){
    std::string needle='"'+key+'"';
    auto pos=body.find(needle); if(pos==std::string::npos)return{};
    pos=body.find(':',pos+needle.size()); if(pos==std::string::npos)return{};
    while(++pos<body.size()&&body[pos]==' ');
    if(pos>=body.size())return{};
    if(body[pos]=='"'){
        ++pos; std::string val;
        while(pos<body.size()&&body[pos]!='"'){
            if(body[pos]=='\\'&&pos+1<body.size()){++pos;val+=(body[pos]=='n')?'\n':body[pos];}
            else val+=body[pos];
            ++pos;
        }
        return val;
    }
    std::string val;
    while(pos<body.size()&&body[pos]!=','&&body[pos]!='}'&&body[pos]!=' ')val+=body[pos++];
    return val;
}

static std::vector<std::string> jArray(const std::string& body, const std::string& key){
    std::vector<std::string> res;
    std::string needle = "\"" + key + "\"";
    auto pos = body.find(needle);
    if(pos == std::string::npos) return res;
    pos = body.find('[', pos + needle.size());
    if(pos == std::string::npos) return res;
    ++pos;
    while(pos < body.size() && body[pos] != ']'){
        while(pos < body.size() && (body[pos] == ' ' || body[pos] == '\t' || body[pos] == '\r' || body[pos] == '\n' || body[pos] == ',')){
            ++pos;
        }
        if(pos >= body.size() || body[pos] == ']') break;
        if(body[pos] == '"'){
            ++pos;
            std::string item;
            while(pos < body.size() && body[pos] != '"'){
                if(body[pos] == '\\' && pos + 1 < body.size()){
                    ++pos;
                    item += (body[pos] == 'n') ? '\n' : body[pos];
                } else {
                    item += body[pos];
                }
                ++pos;
            }
            if(pos < body.size() && body[pos] == '"') ++pos;
            res.push_back(item);
        } else {
            std::string item;
            while(pos < body.size() && body[pos] != ',' && body[pos] != ']') item += body[pos++];
            res.push_back(item);
        }
    }
    return res;
}

// ---------------------------------------------------------------------------
// Thread-safe SSE broadcaster with per-client queues
// ---------------------------------------------------------------------------
struct SSEClient {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::string> queue;
    bool closed = false;

    void push(const std::string& ev, const std::string& data) {
        std::lock_guard<std::mutex> lk(mu);
        if (closed) return;
        queue.push_back("event: " + ev + "\ndata: " + data + "\n\n");
        cv.notify_one();
    }

    void close() {
        std::lock_guard<std::mutex> lk(mu);
        closed = true;
        cv.notify_all();
    }
};

class SSEBroadcaster {
private:
    std::mutex mu_;
    std::vector<std::shared_ptr<SSEClient>> clients_;

public:
    std::shared_ptr<SSEClient> addClient() {
        auto client = std::make_shared<SSEClient>();
        std::lock_guard<std::mutex> lk(mu_);
        clients_.push_back(client);
        return client;
    }

    void removeClient(const std::shared_ptr<SSEClient>& client) {
        if (!client) return;
        client->close();
        std::lock_guard<std::mutex> lk(mu_);
        clients_.erase(std::remove(clients_.begin(), clients_.end(), client), clients_.end());
    }

    void broadcast(const std::string& ev, const std::string& data) {
        std::lock_guard<std::mutex> lk(mu_);
        clients_.erase(std::remove_if(clients_.begin(), clients_.end(),
            [](const std::shared_ptr<SSEClient>& c) { return c->closed; }), clients_.end());
        for (auto& c : clients_) {
            c->push(ev, data);
        }
    }
};

// ---------------------------------------------------------------------------
// LAN IP detection
// ---------------------------------------------------------------------------
static std::string getLanIp(){
#ifdef _WIN32
    WSADATA w;
    if (WSAStartup(MAKEWORD(2,2), &w) == 0) {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s != INVALID_SOCKET) {
            sockaddr_in target{};
            target.sin_family = AF_INET;
            target.sin_port = htons(53);
            inet_pton(AF_INET, "8.8.8.8", &target.sin_addr);
            if (connect(s, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0) {
                sockaddr_in local{};
                int len = sizeof(local);
                if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
                    char buf[INET_ADDRSTRLEN] = {};
                    inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf));
                    closesocket(s);
                    std::string res = buf;
                    if (!res.empty() && res != "0.0.0.0" && res != "127.0.0.1") {
                        return res;
                    }
                }
            }
            closesocket(s);
        }
        // Fallback: enumerate hostname addresses, skipping known virtual host-only ranges
        char host[256]={}; gethostname(host,256);
        struct addrinfo hints{},*res=nullptr; hints.ai_family=AF_INET;
        if(!getaddrinfo(host,nullptr,&hints,&res)&&res){
            char buf[INET_ADDRSTRLEN];
            for(auto* p=res; p; p=p->ai_next){
                inet_ntop(AF_INET,&((sockaddr_in*)p->ai_addr)->sin_addr,buf,sizeof(buf));
                std::string cand = buf;
                if(cand.rfind("192.168.56.", 0) == 0) continue; // VirtualBox host-only
                if(cand.rfind("127.", 0) == 0) continue;
                freeaddrinfo(res);
                return cand;
            }
            inet_ntop(AF_INET,&((sockaddr_in*)res->ai_addr)->sin_addr,buf,sizeof(buf));
            freeaddrinfo(res); return buf;
        }
    }
    return "127.0.0.1";
#else
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s >= 0) {
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(53);
        inet_pton(AF_INET, "8.8.8.8", &target.sin_addr);
        if (connect(s, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0) {
            sockaddr_in local{};
            socklen_t len = sizeof(local);
            if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
                char buf[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf));
                close(s);
                std::string res = buf;
                if (!res.empty() && res != "0.0.0.0" && res != "127.0.0.1") return res;
            }
        }
        close(s);
    }
    struct ifaddrs* ifas=nullptr; getifaddrs(&ifas);
    std::string best;
    for(auto* ifa=ifas;ifa;ifa=ifa->ifa_next){
        if(!ifa->ifa_addr||ifa->ifa_addr->sa_family!=AF_INET)continue;
        if(ifa->ifa_flags&IFF_LOOPBACK)continue;
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET,&((sockaddr_in*)ifa->ifa_addr)->sin_addr,buf,sizeof(buf));
        best=buf; break;
    }
    if(ifas)freeifaddrs(ifas);
    return best.empty()?"127.0.0.1":best;
#endif
}


// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc,char* argv[]){
    const std::string data_dir=(argc>1)?argv[1]:"./data";
    const int port=(argc>2)?std::stoi(argv[2]):8080;

    fs::create_directories(data_dir);
    fs::create_directories(data_dir+"/uploads");

    StorageManager     sm(data_dir); sm.initialize();
    LockManager        locks;
    TransactionManager tm(locks);
    DeadlockDetector   deadlock_detector(locks, tm, std::chrono::milliseconds(500));
    deadlock_detector.Start();
    ConcurrentStorageManager csm(sm,tm,locks);


    SSEBroadcaster sse;
    httplib::Server svr;

    const std::string static_dir =std::string(".")+"/dashboard/dist";
    const std::string uploads_dir=data_dir+"/uploads";

    if(fs::exists(static_dir+"/assets")) svr.set_mount_point("/assets", static_dir+"/assets");
    svr.set_mount_point("/uploads", uploads_dir);

    // Serve non-index root assets (SVG logos, favicons, icons)
    if(fs::exists(static_dir)){
        for(const auto& entry : fs::directory_iterator(static_dir)){
            if(!entry.is_directory() && entry.path().filename() != "index.html"){
                std::string fname = entry.path().filename().string();
                std::string fpath = entry.path().string();
                std::string ext = entry.path().extension().string();
                std::string mime = (ext == ".svg") ? "image/svg+xml" : (ext == ".png") ? "image/png" : "application/octet-stream";
                svr.Get("/" + fname, [fpath, mime](const httplib::Request&, httplib::Response& res){
                    std::ifstream f(fpath, std::ios::binary);
                    std::string c{std::istreambuf_iterator<char>(f),{}};
                    res.status = 200;
                    res.set_content(c, mime);
                });
            }
        }
    }

    auto serveIndexHtml = [&](const httplib::Request&, httplib::Response& res) {
        if(fs::exists(static_dir+"/index.html")){
            std::ifstream f(static_dir+"/index.html");
            std::string html{std::istreambuf_iterator<char>(f),{}};
            res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
            res.set_header("Pragma", "no-cache");
            res.set_header("Expires", "0");
            res.status=200; res.set_content(html, "text/html");
        } else {
            res.status=404; res.set_content("index.html not found", "text/plain");
        }
    };
    svr.Get("/", serveIndexHtml);
    svr.Get("/index.html", serveIndexHtml);

    svr.Options(".*",[](const httplib::Request&,httplib::Response& res){
        res.set_header("Access-Control-Allow-Origin","*");
        res.set_header("Access-Control-Allow-Headers","Content-Type, Authorization");
        res.set_header("Access-Control-Allow-Methods","GET,POST,PUT,DELETE,OPTIONS");
        res.status=204;
    });
    auto cors=[](httplib::Response& r){r.set_header("Access-Control-Allow-Origin","*");};
    const char* json="application/json";

    // Auto-provision hardcoded Admin user account (username: "admin", password: "admin")
    auto ensureAdmin = [&]() {
        auto existing = csm.FindUserById("admin");
        if (!existing) {
            csm.CreateUser("admin", "admin");
            existing = csm.FindUserById("admin");
        }
        if (existing) {
            User u = *existing;
            u.role = "ADMIN";
            if (u.email.empty()) u.email = "admin@walnut.internal";
            // Default password: "admin" (SHA-256 hash sliced to 63 chars for 64-byte buffer)
            u.password_hash = "8c6976e5b5410415bde908bd4dee15dfb167a9c873fc4bb8a81f6f2ab448a91";
            csm.UpdateUser(u);
        }
    };
    ensureAdmin();
    csm.SetEnforceTieredIncrement(false);

    auto getRequestUser = [&](const httplib::Request& req) -> std::optional<User> {
        std::string token;
        if(req.has_header("Authorization")){
            std::string auth_hdr = req.get_header_value("Authorization");
            if(auth_hdr.rfind("Bearer ",0)==0) token=auth_hdr.substr(7);
        }
        std::string uid = token.empty() ? "" : tokenUserId(token);
        if(uid.empty()) return std::nullopt;
        return csm.FindUserById(uid);
    };

    auto isRequestAdmin = [&](const httplib::Request& req) -> bool {
        auto u = getRequestUser(req);
        return u && (u->role == "ADMIN" || u->user_id == "admin");
    };

    // POST /api/users  (legacy – auto-provision by username)
    svr.Post("/api/users",[&](const httplib::Request& req,httplib::Response& res){
        cors(res);
        try{
            std::string username=jField(req.body,"username");
            if(username.empty()){res.status=400;res.set_content(R"({"error":"username required"})",json);return;}
            std::string custom_id=jField(req.body,"user_id");
            std::string uid=custom_id;
            if(uid.empty()){
                uid=username; for(char& c:uid)if(c==' ')c='_';
            }
            auto existing=csm.FindUserById(uid);
            if(existing){
                std::string tok=sessionForUser(uid);
                std::ostringstream o;
                o<<"{\"user_id\":"<<jStr(uid)<<",\"username\":"<<jStr(username)<<",\"email\":"<<jStr(existing->email)<<",\"token\":"<<jStr(tok)<<"}";
                res.status=200; res.set_content(o.str(),json);
                return;
            }
            csm.CreateUser(uid,username);
            std::string tok=sessionForUser(uid);
            std::ostringstream o;
            o<<"{\"user_id\":"<<jStr(uid)<<",\"username\":"<<jStr(username)<<",\"email\":\"\",\"token\":"<<jStr(tok)<<"}";
            res.status=201; res.set_content(o.str(),json);
        }catch(const std::exception& e){res.status=409;res.set_content("{\"error\":"+jStr(e.what())+"}",json);}
    });

    // POST /api/auth/signup
    svr.Post("/api/auth/signup",[&](const httplib::Request& req,httplib::Response& res){
        cors(res);
        try{
            std::string username = jField(req.body,"username");
            std::string email    = jField(req.body,"email");
            std::string pwhash   = jField(req.body,"password"); // SHA-256 hex from browser
            std::string phone    = jField(req.body,"phone");
            if(username.empty()||email.empty()||pwhash.empty()){
                res.status=400;
                res.set_content(R"({"error":"username, email and password are required"})",json);
                return;
            }
            if(pwhash.size() > 63) pwhash = pwhash.substr(0, 63);
            // Derive user_id from username
            std::string uid = username;
            for(char& c : uid) if(c==' ') c='_';
            // Check for existing user
            auto existing = csm.FindUserById(uid);
            if(existing){
                res.status=409;
                res.set_content(R"({"error":"Username already taken."})",json);
                return;
            }
            // Create user
            csm.CreateUser(uid, username);
            // Update extra fields via FindUserById then re-create with email/phone/hash
            // (We patch the record after creation using the storage manager)
            auto created = csm.FindUserById(uid);
            if(created){
                User u = *created;
                u.email         = email;
                u.phone         = phone;
                u.password_hash = pwhash;
                csm.UpdateUser(u);
            }
            std::string tok = sessionForUser(uid);
            std::ostringstream o;
            o<<"{\"user_id\":"<<jStr(uid)<<",\"username\":"<<jStr(username)
             <<",\"email\":"<<jStr(email)<<",\"role\":"<<jStr("USER")<<",\"token\":"<<jStr(tok)<<"}";
            res.status=201; res.set_content(o.str(),json);
        }catch(const std::exception& e){res.status=409;res.set_content("{\"error\":"+jStr(e.what())+"}",json);}
    });

    // POST /api/auth/signin
    svr.Post("/api/auth/signin",[&](const httplib::Request& req,httplib::Response& res){
        cors(res);
        try{
            std::string username = jField(req.body,"username");
            std::string pwhash   = jField(req.body,"password"); // SHA-256 hex from browser
            if(username.empty()||pwhash.empty()){
                res.status=400;
                res.set_content(R"({"error":"username and password required"})",json);
                return;
            }
            if(pwhash.size() > 63) pwhash = pwhash.substr(0, 63);
            std::string uid = username;
            for(char& c : uid) if(c==' ') c='_';
            auto user = csm.FindUserById(uid);
            if(!user){
                if(uid == "admin"){
                    ensureAdmin();
                    user = csm.FindUserById("admin");
                }
            }
            if(!user){
                res.status=401;
                res.set_content(R"({"error":"Invalid credentials: user does not exist."})",json);
                return;
            }
            // Verify password hash (or default admin credentials "admin" / "admin123")
            bool pwOk = true;
            if (uid == "admin") {
                if (pwhash == "admin" || pwhash == "admin123" ||
                    pwhash == "8c6976e5b5410415bde908bd4dee15dfb167a9c873fc4bb8a81f6f2ab448a91" ||
                    pwhash == "8c6976e5b5410415bde908bd4dee15dfb167a9c873fc4bb8a81f6f2ab448a918" ||
                    pwhash == "240be518fabd2724ddb6f04eeb1da5967448d7e831c08c8fa822809f74c720a" ||
                    pwhash == "240be518fabd2724ddb6f04eeb1da5967448d7e831c08c8fa822809f74c720a9") {
                    pwOk = true;
                } else if (!user->password_hash.empty() && user->password_hash != pwhash) {
                    pwOk = false;
                }
            } else if (!user->password_hash.empty()) {
                if (user->password_hash != pwhash) {
                    pwOk = false;
                }
            }
            if(!pwOk){
                res.status=401;
                res.set_content(R"({"error":"Invalid credentials."})",json);
                return;
            }
            std::string tok = sessionForUser(uid);
            std::ostringstream o;
            o<<"{\"user_id\":"<<jStr(uid)<<",\"username\":"<<jStr(user->username)
             <<",\"email\":"<<jStr(user->email)<<",\"role\":"<<jStr(user->role)<<",\"token\":"<<jStr(tok)<<"}";
            res.status=200; res.set_content(o.str(),json);
        }catch(const std::exception& e){res.status=401;res.set_content("{\"error\":"+jStr(e.what())+"}",json);}
    });

    // GET /api/auth/me  (validate token)
    svr.Get("/api/auth/me",[&](const httplib::Request& req,httplib::Response& res){
        cors(res);
        std::string token;
        if(req.has_header("Authorization")){
            std::string auth_hdr = req.get_header_value("Authorization");
            if(auth_hdr.rfind("Bearer ",0)==0) token=auth_hdr.substr(7);
        }
        std::string uid = token.empty() ? "" : tokenUserId(token);
        if(uid.empty()){ res.status=401; res.set_content(R"({"error":"not authenticated"})",json); return; }
        auto user = csm.FindUserById(uid);
        if(!user){ res.status=404; res.set_content(R"({"error":"user not found"})",json); return; }
        res.set_content(userJson(*user),json);
    });

    // POST/PUT /api/auth/profile  (update current user profile)
    auto handleUpdateProfile = [&](const httplib::Request& req, httplib::Response& res){
        cors(res);
        try{
            auto user = getRequestUser(req);
            if(!user){ res.status=401; res.set_content(R"({"error":"Not authenticated"})",json); return; }
            User u = *user;
            std::string new_name  = jField(req.body, "username");
            std::string new_email = jField(req.body, "email");
            std::string new_phone = jField(req.body, "phone");
            if(!new_name.empty())  u.username = new_name;
            if(!new_email.empty()) u.email    = new_email;
            if(!new_phone.empty()) u.phone    = new_phone;
            csm.UpdateUser(u);
            res.status=200; res.set_content(userJson(u), json);
        }catch(const std::exception& e){res.status=500;res.set_content("{\"error\":"+jStr(e.what())+"}",json);}
    };
    svr.Post("/api/auth/profile", handleUpdateProfile);
    svr.Put("/api/auth/profile", handleUpdateProfile);

    // POST /api/auth/change-password
    svr.Post("/api/auth/change-password",[&](const httplib::Request& req,httplib::Response& res){
        cors(res);
        try{
            std::string token;
            if(req.has_header("Authorization")){
                std::string auth_hdr = req.get_header_value("Authorization");
                if(auth_hdr.rfind("Bearer ",0)==0) token=auth_hdr.substr(7);
            }
            std::string uid = token.empty() ? "" : tokenUserId(token);
            if(uid.empty()){ res.status=401; res.set_content(R"({"error":"Not authenticated"})",json); return; }
            auto user = csm.FindUserById(uid);
            if(!user){ res.status=404; res.set_content(R"({"error":"User not found"})",json); return; }

            std::string old_hash = jField(req.body,"old_password");
            std::string new_hash = jField(req.body,"new_password");
            if(old_hash.size() > 63) old_hash = old_hash.substr(0, 63);
            if(new_hash.size() > 63) new_hash = new_hash.substr(0, 63);
            if(new_hash.empty()){
                res.status=400; res.set_content(R"({"error":"New password is required"})",json); return;
            }
            if(!user->password_hash.empty() && user->password_hash != old_hash){
                res.status=401; res.set_content(R"({"error":"Current password does not match."})",json); return;
            }
            User u = *user;
            u.password_hash = new_hash;
            csm.UpdateUser(u);
            res.status=200; res.set_content(R"({"success":true,"message":"Password updated successfully."})",json);
        }catch(const std::exception& e){res.status=500;res.set_content("{\"error\":"+jStr(e.what())+"}",json);}
    });

    // POST /api/auth/reset-password (Recovery / Forgot password)
    svr.Post("/api/auth/reset-password",[&](const httplib::Request& req,httplib::Response& res){
        cors(res);
        try{
            std::string username = jField(req.body,"username");
            std::string email    = jField(req.body,"email");
            std::string new_hash = jField(req.body,"new_password");
            if(new_hash.size() > 63) new_hash = new_hash.substr(0, 63);
            if(username.empty()||new_hash.empty()){
                res.status=400; res.set_content(R"({"error":"Username and new password are required."})",json); return;
            }
            std::string uid = username;
            for(char& c : uid) if(c==' ') c='_';
            auto user = csm.FindUserById(uid);
            if(!user){
                res.status=404; res.set_content(R"({"error":"User handle not found."})",json); return;
            }
            if(!user->email.empty() && !email.empty() && user->email != email){
                res.status=400; res.set_content(R"({"error":"Email does not match registered account."})",json); return;
            }
            User u = *user;
            u.password_hash = new_hash;
            if(!email.empty()) u.email = email;
            csm.UpdateUser(u);
            std::string tok = sessionForUser(uid);
            std::ostringstream o;
            o<<"{\"success\":true,\"user_id\":"<<jStr(uid)<<",\"username\":"<<jStr(u.username)
             <<",\"email\":"<<jStr(u.email)<<",\"token\":"<<jStr(tok)<<"}";
            res.status=200; res.set_content(o.str(),json);
        }catch(const std::exception& e){res.status=500;res.set_content("{\"error\":"+jStr(e.what())+"}",json);}
    });

    // GET /api/users (list all users - admin only)
    svr.Get("/api/users",[&](const httplib::Request& req,httplib::Response& res){
        cors(res);
        try{
            if(!isRequestAdmin(req)){
                res.status=403;
                res.set_content(R"({"error":"Forbidden: Administrator privileges required."})",json);
                return;
            }
            auto users = csm.GetAllUsers();
            std::string b = "[";
            for(size_t i = 0; i < users.size(); ++i){
                if(i) b += ',';
                b += userJson(users[i]);
            }
            res.set_content(b + "]", json);
        }catch(const std::exception& e){res.status=500;res.set_content("{\"error\":"+jStr(e.what())+"}",json);}
    });

    // GET /api/users/:uid
    svr.Get(R"(/api/users/([^/]+))",[&](const httplib::Request& req,httplib::Response& res){
        cors(res);
        auto u=csm.FindUserById(req.matches[1]);
        if(!u){res.status=404;res.set_content(R"({"error":"not found"})",json);return;}
        res.set_content(userJson(*u),json);
    });

    // DELETE /api/users/:uid (admin only)
    svr.Delete(R"(/api/users/([^/]+))",[&](const httplib::Request& req,httplib::Response& res){
        cors(res);
        try{
            if(!isRequestAdmin(req)){
                res.status=403;
                res.set_content(R"({"error":"Forbidden: Administrator privileges required."})",json);
                return;
            }
            std::string uid = req.matches[1];
            if(uid == "admin"){
                res.status=400;
                res.set_content(R"({"error":"Cannot delete primary admin account."})",json);
                return;
            }
            csm.DeleteUser(uid);
            {
                std::lock_guard<std::mutex> lk(g_sessions_mu);
                for(auto it = g_sessions.begin(); it != g_sessions.end(); ){
                    if(it->second == uid) it = g_sessions.erase(it);
                    else ++it;
                }
            }
            sse.broadcast("user_deleted", "{\"user_id\":" + jStr(uid) + "}");
            res.status=200; res.set_content(R"({"success":true,"message":"User deleted."})",json);
        }catch(const std::exception& e){res.status=409;res.set_content("{\"error\":"+jStr(e.what())+"}",json);}
    });

    // GET /api/items (?category=)
    svr.Get("/api/items",[&](const httplib::Request& req,httplib::Response& res){
        cors(res);
        try{
            std::vector<Item> items;
            if(req.has_param("category")&&!req.get_param_value("category").empty())
                items=csm.GetItemsByCategory(req.get_param_value("category"));
            else items=csm.GetAllItems();
            using namespace std::chrono;
            uint64_t now_ms=(uint64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
            for(auto& it : items){
                if(it.end_time_ms > 0 && now_ms >= it.end_time_ms && it.status == "OPEN"){
                    try {
                        csm.OnTimerExpire(it.item_id);
                        it.status = (!it.current_winner.empty() || it.total_bid_count > 0) ? "CLOSED" : "UNSOLD";
                        sse.broadcast("item_closed", itemJson(it));
                    } catch(...) {}
                }
            }
            std::string b="[";
            for(std::size_t i=0;i<items.size();++i){if(i)b+=',';b+=itemJson(items[i]);}
            res.set_content(b+"]",json);
        }catch(const std::exception& e){res.status=500;res.set_content("{\"error\":"+jStr(e.what())+"}",json);}
    });

    // POST /api/items
    svr.Post("/api/items",[&](const httplib::Request& req,httplib::Response& res){
        cors(res);
        try{
            std::string title   =jField(req.body,"title");
            std::string sp_str  =jField(req.body,"starting_price");
            std::string host_id =jField(req.body,"host_id");
            std::string category=jField(req.body,"category");
            std::string cond    =jField(req.body,"condition");
            std::string item_id =jField(req.body,"item_id");
            std::string end_str =jField(req.body,"end_time_ms");
            if(title.empty()||sp_str.empty()){res.status=400;res.set_content(R"({"error":"title and starting_price required"})",json);return;}
            double sp=std::stod(sp_str);
            using namespace std::chrono;
            uint64_t now_ms=(uint64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
            if(item_id.empty()){
                item_id=title.substr(0,std::min((int)title.size(),8));
                for(char& c:item_id)if(!isalnum((unsigned char)c))c='_';
                item_id+="_"+std::to_string(now_ms%100000);
            }

            Item it;
            it.item_id = item_id;
            it.host_id = host_id.empty() ? "host_walnut" : host_id;
            it.title = title;
            it.category = category.empty() ? "GENERAL" : category;
            it.condition = cond.empty() ? "GOOD" : cond;
            it.starting_price = sp;
            it.status = "OPEN";
            it.current_bid = 0.0;
            it.end_time_ms = (!end_str.empty() && std::stoull(end_str) > 0)
                             ? std::stoull(end_str)
                             : (now_ms + 2 * 3600 * 1000);

            // Parse image_urls
            auto img_list = jArray(req.body, "image_urls");
            std::string single_img = jField(req.body, "image_url");
            if(img_list.empty() && !single_img.empty()) img_list.push_back(single_img);

            for(size_t i = 0; i < 3 && i < img_list.size(); ++i){
                std::string u = img_list[i];
                if(u.empty()) continue;
                // If it starts with /uploads/, check if filename or alias fits into ITEM_IMAGE_URL_LEN (32 bytes = max 31 chars)
                if(u.size() >= 32 && u.rfind("/uploads/", 0) == 0){
                    std::string fname = u.substr(9);
                    if(fname.size() < 32){
                        u = fname;
                    } else {
                        // Create a short alias in uploads_dir if file exists
                        std::string ext = "";
                        auto dot = fname.rfind('.');
                        if(dot != std::string::npos) ext = fname.substr(dot);
                        std::string short_name = "s_" + fname.substr(0, 16) + ext;
                        try {
                            if(fs::exists(uploads_dir + "/" + fname) && !fs::exists(uploads_dir + "/" + short_name)){
                                fs::copy_file(uploads_dir + "/" + fname, uploads_dir + "/" + short_name);
                            }
                        } catch(...) {}
                        u = short_name;
                    }
                }
                if(u.size() >= 32) u = u.substr(0, 31);
                it.image_urls[i] = u;
            }

            sm.createItem(it);
            auto created = csm.GetItem(item_id);
            if(created){
                sse.broadcast("new_item", itemJson(*created));
                res.status = 201;
                res.set_content(itemJson(*created), json);
            } else {
                res.status = 500;
                res.set_content(R"({"error":"write failed"})", json);
            }
        }catch(const std::exception& e){res.status=409;res.set_content("{\"error\":"+jStr(e.what())+"}",json);}
    });

    // GET /api/items/won/:uid
    svr.Get(R"(/api/items/won/([^/]+))",[&](const httplib::Request& req,httplib::Response& res){
        cors(res);
        try{
            auto items=csm.GetItemsWonByUser(req.matches[1]);
            std::string b="["; for(std::size_t i=0;i<items.size();++i){if(i)b+=',';b+=itemJson(items[i]);}
            res.set_content(b+"]",json);
        }catch(const std::exception& e){res.status=500;res.set_content("{\"error\":"+jStr(e.what())+"}",json);}
    });

    // GET /api/items/host/:uid
    svr.Get(R"(/api/items/host/([^/]+))",[&](const httplib::Request& req,httplib::Response& res){
        cors(res);
        try{
            auto items=csm.GetItemsByHost(req.matches[1]);
            std::string b="["; for(std::size_t i=0;i<items.size();++i){if(i)b+=',';b+=itemJson(items[i]);}
            res.set_content(b+"]",json);
        }catch(const std::exception& e){res.status=500;res.set_content("{\"error\":"+jStr(e.what())+"}",json);}
    });

    // GET /api/items/category/:cat
    svr.Get(R"(/api/items/category/([^/]+))",[&](const httplib::Request& req,httplib::Response& res){
        cors(res);
        try{
            auto items=csm.GetItemsByCategory(req.matches[1]);
            std::string b="["; for(std::size_t i=0;i<items.size();++i){if(i)b+=',';b+=itemJson(items[i]);}
            res.set_content(b+"]",json);
        }catch(const std::exception& e){res.status=500;res.set_content("{\"error\":"+jStr(e.what())+"}",json);}
    });

    // POST /api/items/:id/cancel
    svr.Post(R"(/api/items/([^/]+)/cancel)",[&](const httplib::Request& req,httplib::Response& res){
        cors(res);
        try{
            csm.CancelAuction(req.matches[1]);
            auto item=csm.GetItem(req.matches[1]);
            res.set_content(item?itemJson(*item):R"({"status":"cancelled"})",json);
        }catch(const std::exception& e){res.status=409;res.set_content("{\"error\":"+jStr(e.what())+"}",json);}
    });

    // POST /api/items/:id/close
    svr.Post(R"(/api/items/([^/]+)/close)",[&](const httplib::Request& req,httplib::Response& res){
        cors(res);
        try{
            csm.CloseItem(req.matches[1]);
            auto item=csm.GetItem(req.matches[1]);
            if(item){
                sse.broadcast("item_closed", itemJson(*item));
            }
            res.set_content(item?itemJson(*item):R"({"status":"closed"})",json);
        }catch(const std::exception& e){res.status=409;res.set_content("{\"error\":"+jStr(e.what())+"}",json);}
    });

    // GET /api/items/:id  (must come after named sub-routes)
    svr.Get(R"(/api/items/([^/]+))",[&](const httplib::Request& req,httplib::Response& res){
        cors(res);
        auto item=csm.GetItem(req.matches[1]);
        if(!item){res.status=404;res.set_content(R"({"error":"not found"})",json);return;}
        res.set_content(itemJson(*item),json);
    });

    // DELETE /api/items/:id (host or admin only)
    svr.Delete(R"(/api/items/([^/]+))",[&](const httplib::Request& req,httplib::Response& res){
        cors(res);
        try{
            std::string id = req.matches[1];
            auto item = csm.GetItem(id);
            if (!item) {
                res.status = 404;
                res.set_content(R"({"error":"Item not found"})", json);
                return;
            }
            auto reqUser = getRequestUser(req);
            bool allowed = reqUser && (reqUser->role == "ADMIN" || reqUser->user_id == "admin" || reqUser->user_id == item->host_id);
            if (!allowed) {
                res.status = 403;
                res.set_content(R"({"error":"Forbidden: Only the lot host or an administrator can delete this item."})", json);
                return;
            }
            csm.DeleteItem(id);
            sse.broadcast("item_deleted", "{\"item_id\":" + jStr(id) + "}");
            res.status=200; res.set_content("{\"success\":true,\"message\":\"Item deleted.\"}",json);
        }catch(const std::exception& e){res.status=409;res.set_content("{\"error\":"+jStr(e.what())+"}",json);}
    });

    // POST /api/bids
    svr.Post("/api/bids",[&](const httplib::Request& req,httplib::Response& res){
        cors(res);
        try{
            std::string item_id=jField(req.body,"item_id");
            std::string user_id=jField(req.body,"user_id");
            std::string amt_s  =jField(req.body,"amount");
            if(item_id.empty()||user_id.empty()||amt_s.empty()){
                res.status=400;res.set_content(R"({"error":"item_id, user_id and amount required"})",json);return;
            }
            double amount=std::stod(amt_s);
            // Auto-provision mobile bidder if not yet registered in database
            if(!csm.FindUserById(user_id)){
                csm.CreateUser(user_id, user_id);
            }
            uint64_t bid_id=csm.PlaceBid(item_id,user_id,amount);
            auto item=sm.getItem(item_id);
            if(item){
                std::ostringstream ev;
                ev<<"{\"item_id\":"<<jStr(item_id)<<",\"amount\":"<<amount
                  <<",\"current_winner\":"<<jStr(item->current_winner)<<",\"bid_id\":"<<bid_id
                  <<",\"title\":"<<jStr(item->title)
                  <<",\"item\":"<<itemJson(*item)<<"}";
                sse.broadcast("new_bid",ev.str());
            }
            std::ostringstream o;
            o<<"{\"bid_id\":"<<bid_id<<",\"item_id\":"<<jStr(item_id)<<",\"user_id\":"<<jStr(user_id)
             <<",\"amount\":"<<amount<<",\"item\":"<<(item?itemJson(*item):"null")<<"}";
            res.status=201; res.set_content(o.str(),json);
        }catch(const std::exception& e){res.status=409;res.set_content("{\"error\":"+jStr(e.what())+"}",json);}
    });

    // GET /api/bids/:item_id
    svr.Get(R"(/api/bids/([^/]+))",[&](const httplib::Request& req,httplib::Response& res){
        cors(res);
        try{
            auto bids=csm.GetBids(req.matches[1]);
            std::string b="["; for(std::size_t i=0;i<bids.size();++i){if(i)b+=',';b+=bidJson(bids[i]);}
            res.set_content(b+"]",json);
        }catch(const std::exception& e){res.status=500;res.set_content("{\"error\":"+jStr(e.what())+"}",json);}
    });

    // POST /api/upload  (5 MB cap, compact filename, MIME whitelist)
    svr.Post("/api/upload",[&](const httplib::Request& req,httplib::Response& res){
        cors(res);
        try{
            if(!req.is_multipart_form_data()){res.status=415;res.set_content(R"({"error":"multipart/form-data required"})",json);return;}
            static const std::vector<std::string> ALLOWED={"image/jpeg","image/png","image/webp","image/gif"};
            static constexpr std::size_t MAX=5*1024*1024;
            for(auto& [name,file]:req.files){
                if(name!="file")continue;
                bool ok=false; for(auto& t:ALLOWED)if(file.content_type.find(t)!=std::string::npos){ok=true;break;}
                if(!ok){res.status=415;res.set_content(R"({"error":"only JPEG, PNG, WebP or GIF allowed"})",json);return;}
                if(file.content.size()>MAX){res.status=413;res.set_content(R"({"error":"file exceeds 5 MB limit"})",json);return;}
                std::string ext=".jpg";
                if(file.content_type.find("png") !=std::string::npos)ext=".png";
                if(file.content_type.find("webp")!=std::string::npos)ext=".webp";
                if(file.content_type.find("gif") !=std::string::npos)ext=".gif";

                // Compact filename <= 18 chars: ts (8 hex) + rnd (4 hex) + ext
                using namespace std::chrono;
                uint32_t ts=(uint32_t)(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count()&0xffffffff);
                static std::mt19937 rng(std::random_device{}());
                uint32_t r=(uint32_t)(rng()&0xffff);
                char fbuf[32];
                snprintf(fbuf,sizeof(fbuf),"img_%08x%04x%s",ts,r,ext.c_str());
                std::string fname=fbuf;

                std::ofstream out(uploads_dir+"/"+fname,std::ios::binary);
                out.write(file.content.data(),(std::streamsize)file.content.size());
                res.status=201; res.set_content("{\"url\":"+jStr("/uploads/"+fname)+"}",json);
                return;
            }
            res.status=400; res.set_content(R"({"error":"no 'file' field found"})",json);
        }catch(const std::exception& e){res.status=500;res.set_content("{\"error\":"+jStr(e.what())+"}",json);}
    });

    // GET /api/stats
    svr.Get("/api/stats",[&](const httplib::Request&,httplib::Response& res){
        cors(res);
        std::ostringstream o;
        o<<"{\"buffer_pool\":{\"hits\":0,\"misses\":0,\"evictions\":0,\"dirty_evictions\":0,\"hit_rate\":0.0}"
         <<",\"storage\":{\"total_pages\":"<<sm.pageCount()<<"}}";
        res.set_content(o.str(),json);
    });

    // ───────────────────────────────────────────────────────────────────────────
    // POST / GET /api/recovery/simulate-live
    // Real C++ ARIES RecoveryManager sandbox simulation for live auction data
    // ───────────────────────────────────────────────────────────────────────────
    auto handleLiveRecoverySim = [&](const httplib::Request& req, httplib::Response& res) {
        cors(res);
        try {
            std::string item_id = req.has_param("item_id") ? req.get_param_value("item_id") : jField(req.body, "item_id");
            std::string inflight_user = req.has_param("inflight_user") ? req.get_param_value("inflight_user") : jField(req.body, "inflight_user");
            std::string inflight_bid_str = req.has_param("inflight_amount") ? req.get_param_value("inflight_amount") : jField(req.body, "inflight_amount");

            // Look up real live item or pick the first available active lot
            std::optional<Item> target_item;
            if (!item_id.empty()) {
                target_item = csm.GetItem(item_id);
            }
            if (!target_item) {
                auto all = csm.GetAllItems();
                if (!all.empty()) {
                    target_item = all[0];
                }
            }

            Item it;
            if (target_item) {
                it = *target_item;
            } else {
                it.item_id = "lot-live-1";
                it.title = "1982 Rolex Submariner (Live Lot)";
                it.starting_price = 1200.0;
                it.current_bid = 1450.0;
                it.current_winner = "Kanak";
                it.host_id = "Admin";
            }

            double committed_price = (it.current_bid > 0.0) ? it.current_bid : it.starting_price;
            std::string committed_winner = it.current_winner.empty() ? it.host_id : it.current_winner;

            double inflight_amount = 0.0;
            if (!inflight_bid_str.empty()) {
                try { inflight_amount = std::stod(inflight_bid_str); } catch (...) {}
            }
            if (inflight_amount <= committed_price) {
                inflight_amount = committed_price + 150.0;
            }
            if (inflight_user.empty()) {
                inflight_user = "Vinayak";
            }

            // Create temporary sandbox directory for pure C++ ARIES execution
            std::string sim_dir = data_dir + "/recovery_sim_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
            fs::create_directories(sim_dir);

            #pragma pack(push, 1)
            struct SimBidState {
                double price;
                char winner[32];
            };
            #pragma pack(pop)

            SimBidState committed_state{ committed_price, {0} };
            std::strncpy(committed_state.winner, committed_winner.c_str(), 31);

            SimBidState inflight_state{ inflight_amount, {0} };
            std::strncpy(inflight_state.winner, inflight_user.c_str(), 31);

            uint32_t pid = 0;
            {
                FileManager fm(sim_dir);
                fm.initialize();
                pid = fm.allocatePage(PAGE_TYPE_BIDS);

                // Simulate STEAL policy: page on disk has the dirty uncommitted inflight state
                Page page = fm.readPage(pid);
                std::memcpy(page.dataRegion(), &inflight_state, sizeof(SimBidState));
                page.updateChecksum();
                fm.writePage(page);
            }

            std::string wal_path = sim_dir + "/walnut.wal";
            {
                LogManager lm(wal_path);

                // 1. Checkpoint
                LogRecord cp_rec;
                cp_rec.txn_id = 0;
                cp_rec.type = LogRecordType::CHECKPOINT;
                lm.AppendLogRecord(cp_rec);

                // 2. Committed Transaction (Tx 101: Committed winner)
                LogRecord b1; b1.txn_id = 101; b1.type = LogRecordType::BEGIN;
                lm.AppendLogRecord(b1);

                LogRecord c1; c1.txn_id = 101; c1.type = LogRecordType::COMMIT;
                lm.AppendLogRecord(c1);

                // 3. In-flight Loser Transaction (Tx 102: Crash before COMMIT)
                LogRecord b2; b2.txn_id = 102; b2.type = LogRecordType::BEGIN;
                uint64_t b2_lsn = lm.AppendLogRecord(b2);

                LogRecord u2;
                u2.txn_id = 102;
                u2.type = LogRecordType::UPDATE;
                u2.page_id = pid;
                u2.offset = 0;
                u2.length = sizeof(SimBidState);
                u2.prev_lsn = b2_lsn;

                std::vector<uint8_t> before_b(sizeof(SimBidState)), after_b(sizeof(SimBidState));
                std::memcpy(before_b.data(), &committed_state, sizeof(SimBidState));
                std::memcpy(after_b.data(), &inflight_state, sizeof(SimBidState));
                u2.before_image = before_b;
                u2.after_image = after_b;

                lm.AppendLogRecord(u2);
                lm.FlushAll();
            }

            // Execute C++ RecoveryManager
            RecoveryManager::RecoveryStats stats;
            std::vector<LogRecord> recovered_records;
            SimBidState final_disk_state{ 0.0, {0} };
            {
                FileManager fm(sim_dir);
                fm.initialize();
                LogManager lm(wal_path);
                MetadataManager mm(sim_dir + "/walnut.meta");
                mm.load();
                mm.meta().checkpoint_lsn = 0;
                mm.save();

                RecoveryManager rm(lm, fm, mm);
                rm.Recover();
                stats = rm.GetStats();

                recovered_records = lm.ReadAllRecords();

                Page p = fm.readPage(pid);
                std::memcpy(&final_disk_state, p.dataRegion(), sizeof(SimBidState));
            }

            // Cleanup sandbox
            std::error_code ec;
            fs::remove_all(sim_dir, ec);

            auto typeToStr = [](LogRecordType t) -> std::string {
                switch(t) {
                    case LogRecordType::BEGIN: return "BEGIN";
                    case LogRecordType::UPDATE: return "UPDATE";
                    case LogRecordType::COMMIT: return "COMMIT";
                    case LogRecordType::ABORT: return "ABORT";
                    case LogRecordType::CLR: return "CLR";
                    case LogRecordType::CHECKPOINT: return "CHECKPOINT";
                    default: return "UNKNOWN";
                }
            };

            // Build JSON output
            std::ostringstream o;
            o << "{"
              << "\"status\":\"SUCCESS\","
              << "\"item_id\":" << jStr(it.item_id) << ","
              << "\"item_title\":" << jStr(it.title) << ","
              << "\"committed_price\":" << committed_price << ","
              << "\"committed_winner\":" << jStr(committed_winner) << ","
              << "\"inflight_user\":" << jStr(inflight_user) << ","
              << "\"inflight_amount\":" << inflight_amount << ","
              << "\"page_id\":" << pid << ","
              << "\"stats\":{"
              << "\"records_analyzed\":" << stats.records_analyzed << ","
              << "\"records_redone\":" << stats.records_redone << ","
              << "\"records_undone\":" << stats.records_undone << ","
              << "\"clrs_written\":" << stats.clrs_written << ","
              << "\"loser_txn_count\":" << stats.loser_txn_count
              << "},"
              << "\"restored_price\":" << final_disk_state.price << ","
              << "\"restored_winner\":" << jStr(final_disk_state.winner) << ","
              << "\"wal_records\":[";

            for (size_t i = 0; i < recovered_records.size(); ++i) {
                if (i > 0) o << ",";
                const auto& r = recovered_records[i];
                o << "{"
                  << "\"lsn\":" << r.lsn << ","
                  << "\"prev_lsn\":" << r.prev_lsn << ","
                  << "\"txn_id\":" << r.txn_id << ","
                  << "\"type\":" << jStr(typeToStr(r.type)) << ","
                  << "\"page_id\":" << r.page_id << ","
                  << "\"undo_next_lsn\":" << r.undo_next_lsn
                  << "}";
            }
            o << "]}";

            res.set_content(o.str(), json);
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content("{\"error\":" + jStr(e.what()) + "}", json);
        }
    };

    svr.Get("/api/recovery/simulate-live", handleLiveRecoverySim);
    svr.Post("/api/recovery/simulate-live", handleLiveRecoverySim);

    // POST /api/admin/reset-db (admin only)
    svr.Post("/api/admin/reset-db",[&](const httplib::Request& req,httplib::Response& res){
        cors(res);
        try{
            if(!isRequestAdmin(req)){
                res.status=403;
                res.set_content(R"({"error":"Forbidden: Administrator privileges required."})",json);
                return;
            }
            std::string mode = jField(req.body, "mode");
            if(mode == "purge_items"){
                csm.PurgeItems();
                sse.broadcast("db_reset", R"({"mode":"purge_items"})");
                res.status=200; res.set_content(R"({"success":true,"message":"All auction lots and bids purged."})",json);
            } else if(mode == "factory_reset"){
                csm.ResetDatabase();
                ensureAdmin(); // Auto-provision clean admin account immediately
                {
                    std::lock_guard<std::mutex> lk(g_sessions_mu);
                    g_sessions.clear();
                }
                std::string new_tok = sessionForUser("admin");
                sse.broadcast("db_reset", R"({"mode":"factory_reset"})");
                std::ostringstream o;
                o << "{\"success\":true,\"message\":\"Database wiped and reset to factory clean state.\",\"token\":"
                  << jStr(new_tok) << "}";
                res.status=200; res.set_content(o.str(),json);
            } else {
                res.status=400; res.set_content(R"({"error":"Invalid mode. Use 'purge_items' or 'factory_reset'."})",json);
            }
        }catch(const std::exception& e){res.status=500;res.set_content("{\"error\":"+jStr(e.what())+"}",json);}
    });

    // GET /api/host-info
    svr.Get("/api/host-info",[&](const httplib::Request&,httplib::Response& res){
        cors(res);
        std::string ip=getLanIp(); char host[256]="walnut-host"; gethostname(host,sizeof(host));
        std::ostringstream o;
        o<<"{\"lan_ip\":"<<jStr(ip)<<",\"port\":"<<port
         <<",\"dashboard_url\":"<<jStr("http://"+ip+":"+std::to_string(port))
         <<",\"hostname\":"<<jStr(host)<<"}";
        res.set_content(o.str(),json);
    });

    // GET /api/stream  (SSE - Thread-safe with per-client queue)
    svr.Get("/api/stream",[&](const httplib::Request&,httplib::Response& res){
        cors(res);
        auto client = sse.addClient();
        res.set_chunked_content_provider("text/event-stream",
            [&sse, client](std::size_t, httplib::DataSink& sink) -> bool {
                std::string ping = ": keep-alive\n\n";
                if (!sink.write(ping.c_str(), ping.size())) {
                    sse.removeClient(client);
                    return false;
                }
                while (true) {
                    std::vector<std::string> to_send;
                    {
                        std::unique_lock<std::mutex> lk(client->mu);
                        client->cv.wait_for(lk, std::chrono::seconds(15), [&]() {
                            return client->closed || !client->queue.empty();
                        });
                        if (client->closed) break;
                        if (!client->queue.empty()) {
                            to_send.swap(client->queue);
                        }
                    }
                    if (to_send.empty()) {
                        std::string ka = ": keep-alive\n\n";
                        if (!sink.write(ka.c_str(), ka.size())) break;
                    } else {
                        bool ok = true;
                        for (const auto& msg : to_send) {
                            if (!sink.write(msg.c_str(), msg.size())) {
                                ok = false;
                                break;
                            }
                        }
                        if (!ok) break;
                    }
                }
                sse.removeClient(client);
                return false;
            });
    });

    // SPA fallback
    svr.set_error_handler([&](const httplib::Request& req,httplib::Response& res){
        if(req.path.rfind("/api/",0)!=0&&fs::exists(static_dir+"/index.html")){
            std::ifstream f(static_dir+"/index.html");
            std::string html{std::istreambuf_iterator<char>(f),{}};
            res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
            res.set_header("Pragma", "no-cache");
            res.set_header("Expires", "0");
            res.status=200; res.set_content(html,"text/html");
        }
    });

    std::string ip=getLanIp();
    std::cout<<"[WALnut] Local   : http://localhost:"<<port<<"\n"
             <<"[WALnut] LAN URL : http://"<<ip<<":"<<port<<"\n"
             <<"[WALnut] Data dir: "<<data_dir<<"\n"
             <<"[WALnut] Frontend: "<<(fs::exists(static_dir)?"ready":"not found - run npm run build in dashboard/")<<"\n"
             <<std::flush;

    if (!svr.listen("0.0.0.0", port)) {
        std::cerr << "[WALnut ERROR] Failed to bind/listen on 0.0.0.0:" << port << " - is the port already in use?\n" << std::flush;
    }
    sm.close();
    return 0;
}