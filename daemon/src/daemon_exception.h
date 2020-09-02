// No icecream ;(

#ifndef _DAEMON_EXCEPTION_H_
#define _DAEMON_EXCEPTION_H_

class DaemonException : public std::exception {
    int code_;

public:
    DaemonException(int exitcode) : exception(), code_(exitcode) {}
    int
    exitcode() const
    {
        return code_;
    }
};

#endif // _DAEMON_EXCEPTION_H_
