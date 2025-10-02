#ifndef FILES_H
#define FILES_H

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
using namespace std;

struct FileInfo
{
    string filename;
    string filepath;
    string group_id;
    long filesize;
    string sha1_full;
    vector<string> piece_hashes;
    unordered_set<string> shared_by;

    bool operator==(const FileInfo &other) const
    {
        return filename==other.filename && filepath==other.filepath && group_id==other.group_id && filesize==other.filesize && sha1_full==other.sha1_full;
    }
};

extern unordered_map<string, vector<FileInfo>> group_files;
extern unordered_map<string, FileInfo> all_files;
extern mutex files_mutex;

string upload_file(const vector<string> &tokens, int client_fd);
string list_files_in_group(const vector<string> &tokens, int client_fd);
string download_file(const vector<string> &tokens, int client_fd);
string stop_share(const vector<string> &tokens, int client_fd);

vector<string> split_file_to_chunks(const string &filepath);
string compute_sha1(const string &filepath);

#endif
