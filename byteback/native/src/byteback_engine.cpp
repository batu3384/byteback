#include "byteback_engine.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace byteback {

Engine::Engine() : version_("0.1.0") {}

Engine::~Engine() = default;

std::string Engine::getVersion() const {
    return version_;
}

bool Engine::isAdministrator() const {
#ifdef _WIN32
    BOOL isAdmin = FALSE;
    PSID adminGroup = NULL;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;

    if (AllocateAndInitializeSid(&ntAuthority, 2,
            SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(NULL, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin != FALSE;
#else
    return geteuid() == 0;
#endif
}

} // namespace byteback
