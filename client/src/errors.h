#ifndef _ERRORS_H_
#define _ERRORS_H_

#include <stdexcept>

class ClientError : public std::runtime_error {
public:
    ClientError(int code, const std::string & what)
        : std::runtime_error(what), errorCode(code)
    {
    }

    const int errorCode;
};

class RemoteError final : public ClientError {
public:
    RemoteError(int code, const std::string & what) : ClientError(code, what) {}
};

#endif // _ERRORS_H_
