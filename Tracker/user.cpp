#include "user.h"
#include "files.h"
#include <algorithm>
#include <arpa/inet.h>
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/time.h>
#include <unistd.h>
using namespace std;

unordered_map<string, string> user_and_password; //username -> password
unordered_map<string, string> user_ports; //username -> peer port
unordered_map<int, string> client_user; //client fd -> username
unordered_map<string, string> group_leader; //group id -> leader username
unordered_map<string, vector<string>> group_members; //group id -> list of member usernames
unordered_map<string, vector<string>> group_requests;//group id -> list of pending join request usernames

mutex user_mutex; // protects user_and_password, user_ports, client_user
mutex group_members_mutex;// protects group_leader, group_members, group_requests

//Function to handle create_user command
string create_user(const vector<string> &tokens)
{
    string response;
    string username, password;
    if(tokens.size()!=3) //Incorrect number of arguments
        response="Usage: create_user <username> <password>";
    else
    {
        //Extract parameters
        username=tokens[1];
        password=tokens[2];

        //Check if username already exists
        lock_guard<mutex> lock(user_mutex);
        if(user_and_password.find(username)!=user_and_password.end()) //username exists
            response="Username already exists";
        else
        {
            //Create new user
            user_and_password[username]=password;
            response="User created successfully";
            cout<<"User created: "<<username<<endl;
        }
    }
    return response;
}

//Function to handle login command
string login(const vector<string> &tokens, int client_fd)
{
    // client_fd = -1 means this is a sync message, not from a client connection
    string response;
    if(tokens.size()!=4)
        response="Usage: login <username> <password>";
    else
    {
        //Extract parameters
        string username, password;
        username=tokens[1];
        password=tokens[2];

        //Extract peer port
        string peer_port=tokens[3];
        lock_guard<mutex> lock(user_mutex);

        //Validate username and password
        if(user_and_password.find(username)==user_and_password.end() || user_and_password[username]!=password)
            return "Invalid username or password";
        
        //Check if already logged in on another connection
        if(client_fd!=-1)
        {
            // Check if user is already logged in
            bool logged=false;
            // Iterate through all client_user mappings
            for(auto &i : client_user)
            {
                // If any mapping has this username, user is already logged in
                if(i.second==username)
                {
                    logged=true;
                    break;
                }
            }
            // If already logged in, reject this login attempt
            if(logged)
                return "User already logged in";

            //Map this client fd to username
            client_user[client_fd]=username;
        }
        //Record the peer port for this user
        user_ports[username]=peer_port;
        response="Login successful";
        cout<<"User logged in: "<<username<<" peer="<<peer_port<<endl;
    }
    return response;
}

//Function to handle logout command
string logout(const vector<string> &tokens, int client_fd)
{
    string response;
    if(tokens.size() != 1)
        response="Usage: logout";
    else
    {
        // client_fd must be valid
        lock_guard<mutex> lock(user_mutex);

        if(client_user.find(client_fd)==client_user.end()) //client fd not found
            response="No active session found";
        else
        {
            //Perform logout
            string username=client_user[client_fd];
            client_user.erase(client_fd);//remove client mapping
            user_ports.erase(username);//remove port mapping
            response="Logout successful ("+username+")";
            cout<<"User logged out: "<<username<<endl;
        }
    }
    return response;
}

// logout by username
string logout_by_username(const string &username)
{
    lock_guard<mutex> lock(user_mutex);
    string response;
    // erase any client_user entries matching this username
    
    //remove all client fds associated with this username
    for(auto it=client_user.begin(); it != client_user.end();)
    {
        if(it->second==username)
            it=client_user.erase(it);
        else
            ++it;
    }
    //remove port mapping
    if(user_ports.find(username) != user_ports.end())
    {
        //remove port mapping
        user_ports.erase(username);
        response="Logout successful ("+username+")";
        cout<<"User logged out: "<<username<<endl;
    }
    else
    {
        response="No active session for "+username;
    }
    return response;
}

//Function to handle create_group command
string create_group(const vector<string> &tokens, int client_fd)
{
    string response;
    string group_id;
    if(tokens.size() != 2)
    {
        response="Usage: create_group <group_id>";
    }
    else
    {
        //Extract parameters
        group_id=tokens[1];

        //Create group if not exists
        lock_guard<mutex> lock(group_members_mutex);
        if(client_user.find(client_fd)==client_user.end()) //not logged in
        {
            response="Please login first";
        }
        else if(group_members.find(group_id) != group_members.end()) //group already exists
        {
            response="Group ID already exists";
        }
        else
        {
            //Create new group with this user as leader
            string username=client_user[client_fd];
            group_leader[group_id]=username; //set group leader
            group_members[group_id].push_back(username); //add leader as first member
            group_files[group_id]={}; //initialize empty file list for group
            response="Group created successfully with ID: "+group_id; //success
            cout<<"Group created: "<<group_id<<" by "<<username<<endl;
        }
    }
    return response;
}

//Function to handle join_group command
string join_group(const vector<string> &tokens, int client_fd)
{
    string response;
    string group_id;
    if(tokens.size() != 2)
    {
        response="Usage: join_group <group_id>";
    }
    else
    {
        //Extract parameters
        group_id=tokens[1];
        lock_guard<mutex> lock(group_members_mutex);
        if(client_user.find(client_fd)==client_user.end()) //not logged in
        {
            response="Please login first";
        }
        else if(group_members.find(group_id)==group_members.end()) //group does not exist
        {
            response="Group ID does not exist";
        }
        else
        {
            //Check if already a member or leader
            string username=client_user[client_fd];
            if(group_leader[group_id]==username) //is leader
            {
                response="You are the leader of this group";
            }
            else if(find(group_members[group_id].begin(), group_members[group_id].end(), username) != group_members[group_id].end()) //is member
            {
                response="You are already a member of this group";
            }
            else
            {
                //Add join request
                group_requests[group_id].push_back(username);
                response="Join request sent to group leader";
                cout<<"Join request: user="<<username<<" group="<<group_id<<endl;
            }
        }
    }
    return response;
}

//Function to handle leave_group command
string list_groups(const vector<string> &tokens, int client_fd)
{
    string response;
    if(tokens.size() != 1)
    {
        response="Usage: list_groups";
    }
    else
    {
        //List all groups
        lock_guard<mutex> lock(group_members_mutex);
        if(client_user.find(client_fd)==client_user.end())//not logged in
        {
            response="Please login first";
        }
        else
        {
            if(group_members.empty()) //no groups exist
            {
                response="No groups available";
            }
            else
            {
                //List all group IDs
                response="Groups:\n";
                for(auto &it : group_members)
                {
                    response += it.first+"\n";
                }
            }
        }
    }
    return response;
}

//Function to handle accept_request command
string accept_request(const vector<string> &tokens, int client_fd)
{
    string response;
    if(tokens.size() != 3)
    {
        response="Usage: accept_request <group_id> <username>";
    }
    else
    {
        //Extract parameters
        string group_id=tokens[1];
        string username=tokens[2];
        lock_guard<mutex> lock(group_members_mutex);
        if(client_user.find(client_fd)==client_user.end()) //not logged in
        {
            response="Please login first";
        }
        else if(group_members.find(group_id)==group_members.end())//group does not exist
        {
            response="Group ID does not exist";
        }
        else if(group_leader[group_id] != client_user[client_fd])//not leader
        {
            response="Only group leader can accept requests";
        }
        else
        {
            //Check if user has requested to join
            auto &requests=group_requests[group_id];

            //Check if username exists in requests
            auto it=find(requests.begin(), requests.end(), username);
            if(it==requests.end()) //not found
            {
                response="No such join request found";
            }
            else
            {
                //Add user to group members
                group_members[group_id].push_back(username);
                requests.erase(it); //remove from requests
                response="User "+username+" added to group "+group_id;
                cout<<"Request accepted: user="<<username<<" group="<<group_id<<endl;
            }
        }
    }
    return response;
}

//Function to handle leave_group command
string leave_group(const vector<string> &tokens, int client_fd)
{
    string response;
    if(tokens.size() != 2)
    {
        response="Usage: leave_group <group_id>";
    }
    else
    {
        //Extract parameters
        string group_id=tokens[1];
        lock_guard<mutex> lock(group_members_mutex);
        if(client_user.find(client_fd)==client_user.end()) //not logged in
        {
            response="Please login first";
        }
        else if(group_members.find(group_id)==group_members.end()) //group does not exist
        {
            response="Group ID does not exist";
        }
        else
        {
            //Check if user is a member or leader
            string username=client_user[client_fd];
            if(group_leader[group_id]==username) //is leader
            {
                if(group_members[group_id].size()==1) //only member
                {
                    //Delete the group entirely
                    group_leader.erase(group_id); //remove leader
                    group_members.erase(group_id); //remove members
                    group_requests.erase(group_id); //remove requests
                    response="You have left and deleted the group "+group_id+" as you were the only member";
                    cout<<"Group deleted: "<<group_id<<" by "<<username<<endl;
                }
                else
                {
                    //Transfer leadership to next member
                    group_leader[group_id]=group_members[group_id][1];//second member becomes leader
                    group_members[group_id].erase(group_members[group_id].begin());//remove leader from members
                    response="You have left the group "+group_id+". Leadership transferred to "+group_leader[group_id];
                    cout<<"Leadership transferred for group "<<group_id<<" to "<<group_leader[group_id]<<endl;
                }
            }
            else if(find(group_members[group_id].begin(), group_members[group_id].end(), username)==group_members[group_id].end()) //not a member
            {
                response="You are not a member of this group";
            }
            else
            {
                //Remove user from members
                auto it=find(group_members[group_id].begin(), group_members[group_id].end(), username);
                group_members[group_id].erase(it); //remove from members
                response="You have left the group "+group_id;
                cout<<"User left group: "<<username<<" group="<<group_id<<endl;
                // Remove files shared by this user in the group
                {
                    // Lock the files_mutex
                    lock_guard<mutex> flock(files_mutex);

                    // Identify peerid
                    string peerid=to_string(client_fd);
                    
                    //If user logged in, use username or port as peer id
                    if(user_ports.count(username))
                        peerid=user_ports[username];

                    // Remove peerid from shared_by for files in this group
                    if(group_files.count(group_id))
                    {
                        for(auto &fi : group_files[group_id])
                        {
                            fi.shared_by.erase(peerid);// remove this peer
                        }

                        // Remove files with no sharers from group_files and all_files
                        auto &vec=group_files[group_id];
                        vec.erase(remove_if(vec.begin(), vec.end(), [&](const FileInfo &fi)
                        {
                            // if no peers left sharing, remove file entirely
                            if(fi.shared_by.empty())
                            {
                                // erase from all_files
                                for(auto it=all_files.begin(); it != all_files.end();)
                                {
                                    // If file is no longer shared, remove it
                                    if(it->second.filename==fi.filename && it->second.group_id==fi.group_id)
                                        it=all_files.erase(it); // remove entry
                                    else
                                        ++it;
                                }
                                cout<<"Removed file "<<fi.filename<<" from group "<<group_id<<" because no sharers remain"<<endl;
                                return true;
                            }
                            return false; }), vec.end());
                    }
                }
            }
        }
    }
    return response;
}

//Function to handle upload_file command
string list_requests(const vector<string> &tokens, int client_fd)
{
    string response;
    if(tokens.size() != 2)
    {
        response="Usage: list_requests <group_id>";
    }
    else
    {
        //Extract parameters
        string group_id=tokens[1];
        string username;
        lock_guard<mutex> lock(group_members_mutex);
        if(client_user.find(client_fd)==client_user.end()) //not logged in
        {
            response="Please login first";
        }
        else
        {
            username=client_user[client_fd]; //not logged in
            if(group_members.find(group_id)==group_members.end()) //group does not exist
            {
                response="Group ID does not exist";
            }
            else if(group_leader[group_id] != username) //not leader
            {
                response="Only group leader can view join requests";
            }
            else
            {
                //List pending requests
                auto &requests=group_requests[group_id];
                if(requests.empty())//no requests
                {
                    response="No pending join requests for group "+group_id;
                }
                else
                {
                    //List all requests
                    response="Pending join requests for group "+group_id+":\n";
                    for(auto &user : requests)
                    {
                        response += user+"\n";
                    }
                }
            }
        }
    }
    return response;
}