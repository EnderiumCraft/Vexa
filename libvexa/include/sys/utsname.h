#ifndef LIBVEXA_SYS_UTSNAME_H
#define LIBVEXA_SYS_UTSNAME_H

#ifdef __cplusplus
extern "C" {
#endif

/* What the system is: "Vexa", the host name, the version, "x86_64". */
struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

int uname(struct utsname *u);

#ifdef __cplusplus
}
#endif

#endif
