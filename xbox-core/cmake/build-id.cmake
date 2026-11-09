# Writes OUT (a source file) with this build's identity: the time and a random tag, new each build,
# so that what a run of a build remembers (a self-test step that killed it, the GPU's start) is
# never taken for another build's (a source file's __DATE__ __TIME__ only changes when it is rebuilt)
string(TIMESTAMP when "%Y-%m-%d %H:%M:%S" UTC)
string(RANDOM LENGTH 6 ALPHABET 0123456789abcdef tag)
file(WRITE "${OUT}" "namespace xb { extern const char* const kBuildId; const char* const kBuildId = \"${when} UTC ${tag}\"; }\n")
