struct DebugStruct
    {
    int debugMember;
    };

int main() { // first line of main function
    static DebugStruct debugObject;
    debugObject.debugMember = 1243;
    return debugObject.debugMember;
    }
