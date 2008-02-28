/*
** Copyright 2007 Logitech. All Rights Reserved.
**
** This file is subject to the Logitech Public Source License Version 1.0. Please see the LICENCE file for details.
*/

#include "common.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/stat.h>
#endif

#define RESOLV_TIMEOUT (2 * 60 * 1000) /* 2 minutes */


/*
 * Some systems do not provide this so that we provide our own. It's not
 * marvelously fast, but it works just fine.
 * (from luasocket)
 */
#ifndef HAVE_INET_ATON
int inet_aton(const char *cp, struct in_addr *inp)
{
    unsigned int a = 0, b = 0, c = 0, d = 0;
    int n = 0, r;
    unsigned long int addr = 0;
    r = sscanf(cp, "%u.%u.%u.%u%n", &a, &b, &c, &d, &n);
    if (r == 0 || n == 0) return 0;
    cp += n;
    if (*cp) return 0;
    if (a > 255 || b > 255 || c > 255 || d > 255) return 0;
    if (inp) {
        addr += a; addr <<= 8;
        addr += b; addr <<= 8;
        addr += c; addr <<= 8;
        addr += d;
        inp->s_addr = htonl(addr);
    }
    return 1;
}
#endif


/* write a string to the pipe fd */
static void write_str(int fd, char *str) {
	size_t len;

	len = strlen(str);
	write(fd, &len, sizeof(len));
	write(fd, str, len);
}


/* read a string to the lua stack from the pipe fd */
static void read_pushstring(lua_State *L, int fd) {
	size_t len;
	char *buf;

	read(fd, &len, sizeof(len));

	if (len == 0) {
		lua_pushnil(L);
	}
	else {
		buf = malloc(len);

		read(fd, buf, len);
		lua_pushlstring(L, buf, len);

		free(buf);
	}
}


static int stat_resolv_conf(void) {
#ifndef _WIN32
	struct stat stat_buf;
	static time_t last_mtime = 0;

	/* check if resolv.conf has changed */
	if (stat("/etc/resolv.conf", &stat_buf) == 0) {
		if (last_mtime != stat_buf.st_mtime) {
			last_mtime = stat_buf.st_mtime;
			return 1;
		}
	}
#endif

	return 0;
}


/* dns resolver thread */
static int dns_resolver_thread(void *p) {
	int fd = (int) p;
	struct hostent *hostent;
	struct in_addr **addr, byaddr;
	char **alias;
	size_t len;
	char *buf;
	char *failed_error = NULL;
	Uint32 failed_timeout = 0;

	while (1) {
		if (read(fd, &len, sizeof(len)) < 0) {
			/* broken pipe */
			return 0;
		}

		buf = malloc(len + 1);
		if (read(fd, buf, len) < 0) {
			/* broken pipe */
			free(buf);
			return 0;
		}
		buf[len] = '\0';


		if (failed_error && !stat_resolv_conf()) {
			Uint32 now = SDL_GetTicks();
			
			if (now - failed_timeout < RESOLV_TIMEOUT) {
				write_str(fd, failed_error);
				continue;
			}
		}
		failed_error = NULL;

		if (inet_aton(buf, &byaddr)) {
			hostent = gethostbyaddr((char *) &byaddr, sizeof(addr), AF_INET);
		}
		else {
			hostent = gethostbyname(buf);
		}
		free(buf);

		if (hostent == NULL) {
			/* error */
			switch (h_errno) {
			case HOST_NOT_FOUND:
				write_str(fd, "Not found");
				break;
			case NO_DATA:
				write_str(fd, "No data");
				break;
			case NO_RECOVERY:
				failed_error = "No recovery";
				failed_timeout = SDL_GetTicks();
				write_str(fd, failed_error);
				break;
			case TRY_AGAIN:
				failed_error = "Try again"; 
				failed_timeout = SDL_GetTicks();
				write_str(fd, failed_error);
				break;
			}
		}
		else {
			write_str(fd, ""); // no error
			write_str(fd, hostent->h_name);

			alias = hostent->h_aliases;
			while (*alias) {
				write_str(fd, *alias);
				alias++;
			}
			write_str(fd, ""); // end of aliases

			addr = (struct in_addr **) hostent->h_addr_list;
			while (*addr) {
				write_str(fd, inet_ntoa(**addr));
				addr++;
			}
			write_str(fd, ""); // end if addrs
		}
	}
}


struct dns_userdata {
	int fd[2];
	SDL_Thread *t;
};


static int jiveL_dns_open(lua_State *L) {
	struct dns_userdata *u;
	int r;

#ifndef _WIN32
	u = lua_newuserdata(L, sizeof(struct dns_userdata));

	r = socketpair(AF_UNIX, SOCK_STREAM, 0, u->fd);
	if (r < 0) {
		return luaL_error(L, "socketpair failed: %s", strerror(r));
	}

	u->t = SDL_CreateThread(dns_resolver_thread, (void *)(u->fd[1]));

	luaL_getmetatable(L, "jive.dns");
	lua_setmetatable(L, -2);

	return 1;
#else
	return 0;
#endif // _WIN32
}


static int jiveL_dns_gc(lua_State *L) {
	struct dns_userdata *u;

	u = lua_touserdata(L, 1);
	close(u->fd[0]);
	close(u->fd[1]);

	return 0;
}


static int jiveL_dns_getfd(lua_State *L) {
	struct dns_userdata *u;

	u = lua_touserdata(L, 1);
	lua_pushinteger(L, u->fd[0]);

	return 1;
}


static int jiveL_dns_read(lua_State *L) {
	struct dns_userdata *u;
	int i, resolved;

	u = lua_touserdata(L, 1);

	/* error? */
	read_pushstring(L, u->fd[0]);
	if (!lua_isnil(L, -1)) {
		lua_pushnil(L);
		lua_insert(L, -2);
		return 2;
	}

	/* read hostent table */
	lua_newtable(L);
	resolved = lua_gettop(L);

	lua_pushstring(L, "name");
	read_pushstring(L, u->fd[0]);
	lua_settable(L, resolved);

	i = 1;
	lua_newtable(L);
	read_pushstring(L, u->fd[0]);
	while (!lua_isnil(L, -1)) {
		lua_rawseti(L, -2, i++);
		read_pushstring(L, u->fd[0]);
	}
	lua_pop(L, 1);
	lua_setfield(L, resolved, "alias");

	i = 1;
	lua_newtable(L);
	read_pushstring(L, u->fd[0]);
	while (!lua_isnil(L, -1)) {
		lua_rawseti(L, -2, i++);
		read_pushstring(L, u->fd[0]);
	}
	lua_pop(L, 1);
	lua_setfield(L, resolved, "ip");

	return 1;
}


static int jiveL_dns_write(lua_State *L) {
	struct dns_userdata *u;
	const char *buf;
	size_t len;

	u = lua_touserdata(L, 1);
	buf = lua_tolstring(L, 2, &len);

	write(u->fd[0], &len, sizeof(len));
	write(u->fd[0], buf, len);

	return 0;
}


static const struct luaL_Reg dns_lib[] = {
	{ "open", jiveL_dns_open },
	{ NULL, NULL }
};


int luaopen_jive_net_dns(lua_State *L) {
	luaL_newmetatable(L, "jive.dns");

	lua_pushcfunction(L, jiveL_dns_gc);
	lua_setfield(L, -2, "__gc");

	lua_pushcfunction(L, jiveL_dns_read);
	lua_setfield(L, -2, "read");

	lua_pushcfunction(L, jiveL_dns_write);
	lua_setfield(L, -2, "write");

	lua_pushcfunction(L, jiveL_dns_getfd);
	lua_setfield(L, -2, "getfd");

	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");

	luaL_register(L, "jive.dns", dns_lib);

	return 0;
}