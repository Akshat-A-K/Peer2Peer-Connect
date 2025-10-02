#ifndef USER_H
#define USER_H

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <mutex>
#include <set>
using namespace std;

//Data structures for user management
extern unordered_map<string, string> user_and_password; //username -> password
extern unordered_map<string, string> user_ports;//username -> peer port
extern unordered_map<int, string> client_user;//client fd -> username
extern mutex user_mutex;// protects user_and_password, user_ports, client_user
extern unordered_map<string, string> group_leader;//group id -> leader username
extern unordered_map<string, vector<string>> group_members;//group id -> list of member usernames
extern unordered_map<string, vector<string>> group_requests;//group id -> list of pending join request usernames
extern mutex group_members_mutex;// protects group_leader, group_members, group_requests

//Function declarations
string create_user(const vector<string> &tokens);//Function to handle create_user command
string login(const vector<string> &tokens, int client_fd); //Function to handle login command
string logout(const vector<string> &tokens, int client_fd);//Function to handle logout command
string logout_by_username(const string &username);//Function to logout by username (used in sync)
string create_group(const vector<string> &tokens, int client_fd);//Function to handle create_group command
string join_group(const vector<string> &tokens, int client_fd);//Function to handle join_group command
string leave_group(const vector<string> &tokens, int client_fd);//Function to handle leave_group command
string list_groups(const vector<string> &tokens, int client_fd);//Function to handle list_groups command
string list_requests(const vector<string> &tokens, int client_fd);//Function to handle list_requests command
string accept_request(const vector<string> &tokens, int client_fd);//Function to handle accept_request command

#endif
