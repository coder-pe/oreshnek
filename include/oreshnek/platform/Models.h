// oreshnek/include/oreshnek/platform/Models.h
//
// Plain data models shared by every database backend. Kept free of any database
// client dependency so both SqliteBackend and PgBackend (and future backends)
// can map their rows into the same structs.
#ifndef ORESHNEK_PLATFORM_MODELS_H
#define ORESHNEK_PLATFORM_MODELS_H

#include <string>
#include <vector>

namespace Oreshnek {
namespace Platform {

struct User {
    int id = 0; // 0 means "not found / invalid"
    std::string username;
    std::string email;
    std::string password_hash;
    std::string role; // "admin", "instructor", "student"
    std::string created_at;
    bool is_active = true;
};

struct Video {
    int id = 0;
    std::string title;
    std::string description;
    std::string filename;
    std::string thumbnail;
    int user_id = 0;
    std::string category;
    std::vector<std::string> tags;
    int views = 0;
    int likes = 0;
    std::string created_at;
    std::string duration;
    bool is_public = true;
};

struct Comment {
    int id = 0;
    int video_id = 0;
    int user_id = 0;
    std::string content;
    std::string created_at;
    int parent_id = 0; // For nested replies.
};

}  // namespace Platform
}  // namespace Oreshnek

#endif  // ORESHNEK_PLATFORM_MODELS_H
