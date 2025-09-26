#ifndef USER_H
#define USER_H

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <mutex>
#include <set>
using namespace std;

extern unordered_map<string, string> user_and_password;
extern unordered_map<string, string> user_ports; 
extern unordered_map<int, string> client_user;
extern mutex user_mutex;
extern unordered_map<string, string> group_leader;
extern unordered_map<string, vector<string>> group_members;
extern unordered_map<string, vector<string>> group_requests;
extern mutex group_members_mutex;


string create_user(const vector<string> &tokens);
string login(const vector<string> &tokens, int client_fd);
string logout(const vector<string> &tokens, int client_fd);
string create_group(const vector<string> &tokens, int client_fd);
string join_group(const vector<string> &tokens, int client_fd);
string leave_group(const vector<string> &tokens, int client_fd);
string list_groups(const vector<string> &tokens, int client_fd);
string list_requests(const vector<string> &tokens, int client_fd);
string accept_request(const vector<string> &tokens, int client_fd);

#endif
