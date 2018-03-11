#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string>
#include <sstream>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>

#include "file_util.h"


using namespace std;

/**
 * Adapted from an answer by "Evan Teran" from this stack overflow question:
 * http://stackoverflow.com/questions/236129/split-a-string-in-c
 */
vector<string> split(const string &s, char delim) {
    vector<string> elems;
    stringstream ss(s);
    string item;
    while (getline(ss, item, delim)) {
        if (!item.empty()) {
            elems.push_back(item);
        }
    }
    return elems;
}

/**
 * Adapted from an answer by "dash-tom-bang" from this stack overflow question:
 * http://stackoverflow.com/questions/5772992/get-relative-path-from-two-absolute-paths
 */
string get_relative_path(const string &to, const string &from) {
    vector<string> to_dirs = split(to, '/');
    vector<string> from_dirs = split(from, '/');

    string output;
    output.reserve(to.size());

    vector<string>::const_iterator to_it = to_dirs.begin(),
                                   to_end = to_dirs.end(),
                                   from_it = from_dirs.begin(),
                                   from_end = from_dirs.end();

    while ((to_it != to_end) && (from_it != from_end) && *to_it == *from_it) {
         ++to_it;
         ++from_it;
    }

    while (from_it != from_end) {
        output += "../";
        ++from_it;
    }

    while (to_it != to_end) {
        output += *to_it;
        ++to_it;

        if (to_it != to_end) {
            output += "/";
        }
    }

    return output;
}

/**
 * Returns a path string with as many '..' and '.' components
 * resolved as is possible, unambiguously.  This does not include
 * symlink resolution.  If the input ends in '/', the output will
 * also.
 */
string get_normalized_path(const string &path)
{
    if (path.empty()) {
        return path;
    }

    vector<string> components = split(path, '/');
    int up = 0; // Number of ..'s we still have to process.

    // Convert resolved components to "".
    for (int i=components.size()-1; i>=0; i--) {
        if (components[i] == ".") {
            components[i] = "";
        } else if (components[i] == "..") {
            components[i] = "";
            up++;
        } else if (up > 0 && components[i] != "") {
            components[i] = "";
            up--;
        }
    }

    string result = "";

    // If we were unable to resolve all '..' components, prepend
    // the number that were unresolved.
    if (path[0] != '/' && up > 0) {
        result += "..";
        up--;
        for (; up>0; up--) {
            result += "/..";
        }
    }

    // Add back all components that weren't "resolved-away".
    for (unsigned int i=0; i<components.size(); i++) {
        if (components[i] != "") {
            if (result != "") {
                result += "/";
            }
            result += components[i];
        }
    }

    // Ensure that leading and trailing slashes are retained.
    if (path[0] == '/' && result[0] != '/') {
        result = "/" + result;
    }
    if (path[path.size() - 1] == '/' && result[result.size()-1] != '/') {
        result += "/";
    }

    return result;
}

/**
 * Return an absolute normalized path with leading and trailing slashes
 * preserved, unless the input is empty- in which case return the input.
 * unchanged.
 */
string get_abs_path(const string &path)
{
   if (path.empty()) {
        return path;
    }

    string process_me = path;
    if (path[0] != '/') {
        process_me = get_cwd() + '/' + process_me;
    }

    return get_normalized_path(process_me);
}

/**
 * Returns a string without '..' and '.', and without a trailing '/'.
 *
 * Preconditions:  path must be an absolute path
 * Postconditions: if path is empty or not an absolute path, return original
 *                 path, otherwise, return path after resolving '..' and '.'
 *                 and without a trailing '/'.
 */
string get_canonicalized_path(const string &path) {
    if (path.empty() || path[0] != '/') {
        return path;
    }

  string r = get_normalized_path(path);
  if (r[r.size()-1] == '/') {
    r = r.substr(0, r.size()-1);
  }

  return r;
}

/**
 * Adapted from an answer by "Mark" from this stack overflow question:
 * http://stackoverflow.com/questions/675039/how-can-i-create-directory-tree-in-c-linux
 */
bool mkpath(const string &path) {
    bool success = false;
    int ret = mkdir(path.c_str(), 0775);
    if(ret == -1) {
        switch(errno) {
            case ENOENT:
                if(mkpath(path.substr(0, path.find_last_of('/'))))
                    success = 0 == mkdir(path.c_str(), 0775);
                else
                    success = false;
                break;
            case EEXIST:
                success = true;
                break;
            default:
                success = false;
                break;
        }
    }
    else {
        success = true;
    }

    return success;
}

/**
 * Adapted from an answer by "asveikau" from this stack overflow question:
 * http://stackoverflow.com/questions/2256945/removing-a-non-empty-directory-programmatically-in-c-or-c
 */
bool rmpath(const char* path) {
    DIR *d = opendir(path);
    size_t path_len = strlen(path);
    int r = -1;

    if (d) {
        struct dirent *p;

        r = 0;

        while (!r && (p=readdir(d))) {
            int r2 = -1;
            char *buf;
            size_t len;

            /* Skip the names "." and ".." as we don't want to recurse on them. */
            if (!strcmp(p->d_name, ".") || !strcmp(p->d_name, "..")) {
                continue;
            }

            len = path_len + strlen(p->d_name) + 2;
            buf = (char*)malloc(len);

            if (buf) {
                struct stat statbuf;

                snprintf(buf, len, "%s/%s", path, p->d_name);

                if (!stat(buf, &statbuf)) {
                    if (S_ISDIR(statbuf.st_mode)) {
                        r2 = rmpath(buf);
                    }
                    else {
                        r2 = unlink(buf);
                    }
                }

                free(buf);
            }

            r = r2;
        }

        closedir(d);
    }

    if (!r) {
        r = rmdir(path);
    }

    return r;
}

string get_cwd()
{
    static std::vector<char> buffer(1024);

    errno = 0;
    while (getcwd(&buffer[0], buffer.size() - 1) == 0 && errno == ERANGE) {
        buffer.resize(buffer.size() + 1024);
        errno = 0;
    }
    if (errno != 0)
        return std::string();

    return string(&buffer[0]);
}
