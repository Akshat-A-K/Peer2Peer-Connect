#include "user.h"
#include "files.h"
#include <algorithm>
#include <arpa/inet.h>
using namespace std;

unordered_map<string, string> user_and_password;
unordered_map<string, string> user_ports;
unordered_map<int, string> client_user;
unordered_map<string, string> group_leader;
unordered_map<string, vector<string>> group_members;
unordered_map<string, vector<string>> group_requests;

mutex user_mutex;
mutex group_members_mutex;

string create_user(const vector<string> &tokens)
{
    string response;
    string username, password;
    if (tokens.size() != 3)
    {
        response = "Usage: create_user <username> <password>";
    }
    else
    {
        username = tokens[1];
        password = tokens[2];

        lock_guard<mutex> lock(user_mutex);
        if (user_and_password.find(username) != user_and_password.end())
        {
            response = "Username already exists";
        }
        else
        {
            user_and_password[username] = password;
            response = "User created successfully";
        }
    }
    return response;
}

string login(const vector<string> &tokens, int client_fd)
{
    string response;
    if (tokens.size() != 4)
    {
        response = "Usage: login <username> <password>";
    }
    else
    {
        string username, password;
        username = tokens[1];
        password = tokens[2];
        string peer_port = tokens[3];

        lock_guard<mutex> lock(user_mutex);
        if (user_and_password.find(username) == user_and_password.end() || user_and_password[username] != password)
        {
            return "Invalid username or password";
        }
        if (client_fd != -1)
        {
            bool logged = false;
            for (auto &i : client_user)
            {
                if (i.second == username)
                {
                    logged = true;
                    break;
                }
            }
            if (logged)
            {
                return "User already logged in";
            }
            client_user[client_fd] = username;
        }
        user_ports[username] = peer_port;
        response = "Login successful";
    }
    return response;
}

string logout(const vector<string> &tokens, int client_fd)
{
    string response;
    if (tokens.size() != 1)
    {
        response = "Usage: logout";
    }
    else
    {
        lock_guard<mutex> lock(user_mutex);
        if (client_user.find(client_fd) == client_user.end())
        {
            response = "No active session found";
        }
        else
        {
            string username = client_user[client_fd];
            client_user.erase(client_fd);
            user_ports.erase(username);
            response = "Logout successful (" + username + ")";
        }
    }
    return response;
}

string create_group(const vector<string> &tokens, int client_fd)
{
    string response;
    string group_id;
    if (tokens.size() != 2)
    {
        response = "Usage: create_group <group_id>";
    }
    else
    {
        group_id = tokens[1];
        lock_guard<mutex> lock(group_members_mutex);
        if (client_user.find(client_fd) == client_user.end())
        {
            response = "Please login first";
        }
        else if (group_members.find(group_id) != group_members.end())
        {
            response = "Group ID already exists";
        }
        else
        {
            string username = client_user[client_fd];
            group_leader[group_id] = username;
            group_members[group_id].push_back(username);
            group_files[group_id] = {};
            response = "Group created successfully with ID: " + group_id;
        }
    }
    return response;
}

string join_group(const vector<string> &tokens, int client_fd)
{
    string response;
    string group_id;
    if (tokens.size() != 2)
    {
        response = "Usage: join_group <group_id>";
    }
    else
    {
        group_id = tokens[1];
        lock_guard<mutex> lock(group_members_mutex);
        if (client_user.find(client_fd) == client_user.end())
        {
            response = "Please login first";
        }
        else if (group_members.find(group_id) == group_members.end())
        {
            response = "Group ID does not exist";
        }
        else
        {
            string username = client_user[client_fd];
            if (group_leader[group_id] == username)
            {
                response = "You are the leader of this group";
            }
            else if (find(group_members[group_id].begin(), group_members[group_id].end(), username) != group_members[group_id].end())
            {
                response = "You are already a member of this group";
            }
            else
            {
                group_requests[group_id].push_back(username);
                response = "Join request sent to group leader";
            }
        }
    }
    return response;
}

string list_groups(const vector<string> &tokens, int client_fd)
{
    string response;
    if (tokens.size() != 1)
    {
        response = "Usage: list_groups";
    }
    else
    {
        lock_guard<mutex> lock(group_members_mutex);
        if (client_user.find(client_fd) == client_user.end())
        {
            response = "Please login first";
        }
        else
        {
            if (group_members.empty())
            {
                response = "No groups available";
            }
            else
            {
                response = "Groups:\n";
                for (auto &it : group_members)
                {
                    response += it.first + "\n";
                }
            }
        }
    }
    return response;
}

string accept_request(const vector<string> &tokens, int client_fd)
{
    string response;
    if (tokens.size() != 3)
    {
        response = "Usage: accept_request <group_id> <username>";
    }
    else
    {
        string group_id = tokens[1];
        string username = tokens[2];
        lock_guard<mutex> lock(group_members_mutex);
        if (client_user.find(client_fd) == client_user.end())
        {
            response = "Please login first";
        }
        else if (group_members.find(group_id) == group_members.end())
        {
            response = "Group ID does not exist";
        }
        else if (group_leader[group_id] != client_user[client_fd])
        {
            response = "Only group leader can accept requests";
        }
        else
        {
            auto &requests = group_requests[group_id];
            auto it = find(requests.begin(), requests.end(), username);
            if (it == requests.end())
            {
                response = "No such join request found";
            }
            else
            {
                group_members[group_id].push_back(username);
                requests.erase(it);
                response = "User " + username + " added to group " + group_id;
            }
        }
    }
    return response;
}

string leave_group(const vector<string> &tokens, int client_fd)
{
    string response;
    if (tokens.size() != 2)
    {
        response = "Usage: leave_group <group_id>";
    }
    else
    {
        string group_id = tokens[1];
        lock_guard<mutex> lock(group_members_mutex);
        if (client_user.find(client_fd) == client_user.end())
        {
            response = "Please login first";
        }
        else if (group_members.find(group_id) == group_members.end())
        {
            response = "Group ID does not exist";
        }
        else
        {
            string username = client_user[client_fd];
            if (group_leader[group_id] == username)
            {
                if (group_members[group_id].size() == 1)
                {
                    group_leader.erase(group_id);
                    group_members.erase(group_id);
                    group_requests.erase(group_id);
                    response = "You have left and deleted the group " + group_id + " as you were the only member";
                }
                else
                {
                    group_leader[group_id] = group_members[group_id][1];
                    group_members[group_id].erase(group_members[group_id].begin());
                    response = "You have left the group " + group_id + ". Leadership transferred to " + group_leader[group_id];
                }
            }
            else if (find(group_members[group_id].begin(), group_members[group_id].end(), username) == group_members[group_id].end())
            {
                response = "You are not a member of this group";
            }
            else
            {
                auto it = find(group_members[group_id].begin(), group_members[group_id].end(), username);
                group_members[group_id].erase(it);
                response = "You have left the group " + group_id;
            }
        }
    }
    return response;
}

string list_requests(const vector<string> &tokens, int client_fd)
{
    string response;
    if (tokens.size() != 2)
    {
        response = "Usage: list_requests <group_id>";
    }
    else
    {
        string group_id = tokens[1];
        string username;
        lock_guard<mutex> lock(group_members_mutex);
        if (client_user.find(client_fd) == client_user.end())
        {
            response = "Please login first";
        }
        else
        {
            username = client_user[client_fd];
            if (group_members.find(group_id) == group_members.end())
            {
                response = "Group ID does not exist";
            }
            else if (group_leader[group_id] != username)
            {
                response = "Only group leader can view join requests";
            }
            else
            {
                auto &requests = group_requests[group_id];
                if (requests.empty())
                {
                    response = "No pending join requests for group " + group_id;
                }
                else
                {
                    response = "Pending join requests for group " + group_id + ":\n";
                    for (auto &user : requests)
                    {
                        response += user + "\n";
                    }
                }
            }
        }
    }
    return response;
}