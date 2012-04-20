/**
 * \file ssh.c
 * \author Radek Krejci <rkrejci@cesnet.cz>
 * \brief Implementation of functions to connect to NETCONF server via SSH2.
 *
 * Copyright (C) 2012 CESNET, z.s.p.o.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of the Company nor the names of its contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * ALTERNATIVELY, provided that this notice is retained in full, this
 * product may be distributed under the terms of the GNU General Public
 * License (GPL) version 2 or later, in which case the provisions
 * of the GPL apply INSTEAD OF those given above.
 *
 * This software is provided ``as is, and any express or implied
 * warranties, including, but not limited to, the implied warranties of
 * merchantability and fitness for a particular purpose are disclaimed.
 * In no event shall the company or contributors be liable for any
 * direct, indirect, incidental, special, exemplary, or consequential
 * damages (including, but not limited to, procurement of substitute
 * goods or services; loss of use, data, or profits; or business
 * interruption) however caused and on any theory of liability, whether
 * in contract, strict liability, or tort (including negligence or
 * otherwise) arising in any way out of the use of this software, even
 * if advised of the possibility of such damage.
 *
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <pwd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "libssh2.h"

#include "ssh.h"
#include "messages.h"
#include "session.h"
#include "netconf_internal.h"
#include "messages_internal.h"

struct auth_pref_couple
{
	NC_SSH_AUTH_TYPE type;
	short int value;
};

#define AUTH_COUNT 3
static struct auth_pref_couple sshauth_pref[AUTH_COUNT] = {
		{ NC_SSH_AUTH_INTERACTIVE, 3 },
		{ NC_SSH_AUTH_PASSWORD, 2 },
		{ NC_SSH_AUTH_PUBLIC_KEYS, 1 }
};

/* number of characters to store short number */
#define SHORT_INT_LENGTH 6

void nc_ssh_pref(NC_SSH_AUTH_TYPE type, short int preference)
{
	int dir = 0;
	unsigned short i;
	struct auth_pref_couple new, aux;

	new.type = type;
	new.value = preference;

	for (i = 0; i < AUTH_COUNT; i++) {
		if (sshauth_pref[i].type == new.type) {
			if (sshauth_pref[i].value < new.value) {
				sshauth_pref[i] = new;
				dir = -1;
				/* correct order */
				while ((i + dir) >= 0) {
					if (sshauth_pref[i].value >= sshauth_pref[i + dir].value) {
						aux = sshauth_pref[i + dir];
						sshauth_pref[i + dir] = sshauth_pref[i];
						sshauth_pref[i] = aux;
						i += dir;
					} else {
						break; /* WHILE */
					}
				}
			} else if (sshauth_pref[i].value > new.value) {
				sshauth_pref[i] = new;
				dir = 1;
				/* correct order */
				while ((i + dir) <= AUTH_COUNT) {
					if (sshauth_pref[i].value < sshauth_pref[i + dir].value) {
						aux = sshauth_pref[i + dir];
						sshauth_pref[i + dir] = sshauth_pref[i];
						sshauth_pref[i] = aux;
						i += dir;
					} else {
						break; /* WHILE */
					}
				}
			}
			break; /* FOR */
		}
	}
}

/**
 * @brief Compare two sets of capabilities and return intersection
 *
 * @param[in]	cap_list_x	array of strings idetifying X capabilities
 * @param[in]	cap_list_y	array of strings idetifying Y capabilities
 * @param[out]	version		accepted NETCONF version
 *
 * return		array of strings, identifying common capabilities
 */
char** nc_merge_capabilities(char ** cap_list_x, char ** cap_list_y, int *version)
{
	int c = 0, i, j;
	(*version) = NETCONFVUNK;
	char ** result;

	/* count max size of the resulting list */
	for (i = 0; cap_list_x[i] != NULL; i++)
		;
	for (j = 0; cap_list_y[j] != NULL; j++)
		;
	c = (j > i) ? j : i;

	if ((result = malloc((c + 1) * sizeof(char*))) == NULL) {
		ERROR("Memory allocation failed: %s (%s:%d).", strerror (errno), __FILE__, __LINE__);
		return (NULL);
	}

	c = 0;
	for (i = 0; cap_list_x[i] != NULL; i++) {
		for (j = 0; cap_list_y[j] != NULL; j++) {
			if (strcmp(cap_list_x[i], cap_list_y[j]) == 0) {
				result[c++] = strdup(cap_list_x[i]);
				break;
			}
		}
	}
	result[c] = NULL;

	for (i = 0; result[i] != NULL; i++) { /* Try to find netconf base capability 1.0 */
		if (strcmp(NC_CAP_BASE10_ID, result[i]) == 0) {
			(*version) = NETCONFV10;
			break;
		}
	}

	for (i = 0; result[i] != NULL; i++) { /* Try to find netconf base capability 1.1. Prefered. */
		if (strcmp(NC_CAP_BASE11_ID, result[i]) == 0) {
			(*version) = NETCONFV11;
			break;
		}
	}

	if ((*version) == NETCONFVUNK) {
		ERROR("No base capability found in capabilities intersection.");
		return (NULL);
	}

	return (result);
}

char** nc_parse_hello(struct nc_msg *msg, struct nc_session *session)
{
	xmlNodePtr node, capnode;
	char *cap = NULL, *str = NULL;
	char **capabilities = NULL;
	int c;

	if ((node = xmlDocGetRootElement(msg->doc)) == NULL) {
		ERROR("Parsing <hello> message failed - document is empty.");
		return (NULL);
	}

	if (xmlStrcmp(node->name, BAD_CAST "hello")) {
		ERROR("Parsing <hello> message failed - received non-<hello> message.");
		return (NULL);
	}

	for (node = node->children; node != NULL; node = node->next) {
		if (xmlStrcmp(node->name, BAD_CAST "capabilities") == 0) {
			/* <capabilities> node */

			/* count capabilities */
			for (capnode = node->children, c = 0; capnode != NULL;
			                capnode = capnode->next) {
				c++;
			}

			/* allocate memory for the capability list */
			if ((capabilities = malloc((c + 1) * sizeof(char*))) == NULL) {
				ERROR("Memory allocation failed - %s (%s:%d).", strerror (errno), __FILE__, __LINE__);
				return (NULL);
			}

			/* fill up capabilities list */
			for (capnode = node->children, c = 0; capnode != NULL;
			                capnode = capnode->next) {
				str = (char*) xmlNodeGetContent(capnode);
				if ((str == NULL) || (cap = nc_clrwspace(str)) == NULL) {
					ERROR("Parsing <hello> message failed - unable to read capabilities.");
					return (NULL);
				}
				xmlFree(BAD_CAST str);
				if (strlen(cap) > 0) {
					capabilities[c++] = cap;
				}
			}
			/* list termination NULL */
			capabilities[c] = NULL;
		} else if (xmlStrcmp(node->name, BAD_CAST "session-id") == 0) {
			/* session-id - available only if the caller of this parsing is a client */
			if (session->session_id[0] == '\0') {
				str = (char*) xmlNodeGetContent(node);
				if (strlen(str) >= (size_t) SID_SIZE) {
					/* Session ID is too long and we can not store it */
					ERROR("Received <session-id> is too long - terminating the session.");
					return (NULL);
				}
				strncpy(session->session_id, str, SID_SIZE - 1);
				xmlFree(BAD_CAST str);
			} else {
				/* as defined by RFC, we have to terminate the session */
				ERROR("Received <hello> message with <session-id> - terminating the session.");
				return (NULL);
			}
		} else {
			/* something unknown - log it now, maybe in future we will more strict and will be returning error */
			WARN("Unknown content of the <hello> message (%s), ignoring and trying to continue.", (char*) node->name);
		}
	}

	if (capabilities == NULL || capabilities[0] == NULL) {
		/* no capability received */
		ERROR("Parsing <hello> message failed - no capabilities detected.");
		return (NULL);
	}

	/* everything OK, return received list of supported capabilities */
	return (capabilities);
}

int nc_handshake(struct nc_session *session, char** cpblts, nc_rpc *hello)
{
	int retval = EXIT_SUCCESS;
	int i;
	nc_reply *recv_hello = NULL;
	char **recv_cpblts = NULL, **merged_cpblts = NULL;

	if (nc_session_send_rpc(session, hello) == 0) {
		return (EXIT_FAILURE);
	}

	nc_session_recv_reply(session, &recv_hello);
	if (recv_hello == NULL) {
		return (EXIT_FAILURE);
	}

	if ((recv_cpblts = nc_parse_hello((struct nc_msg*) recv_hello, session)) == NULL) {
		nc_reply_free(recv_hello);
		return (EXIT_FAILURE);
	}
	nc_reply_free(recv_hello);

	if ((merged_cpblts = nc_merge_capabilities(cpblts, recv_cpblts, &(session->version))) == NULL) {
		retval = EXIT_FAILURE;
	} else if ((session->capabilities = nc_cpblts_new(merged_cpblts)) == NULL) {
		retval = EXIT_FAILURE;
	}

	if (recv_cpblts) {
		i = 0;
		while (recv_cpblts[i]) {
			free(recv_cpblts[i]);
			i++;
		}
		free(recv_cpblts);
	}

	if (merged_cpblts) {
		i = 0;
		while (merged_cpblts[i]) {
			free(merged_cpblts[i]);
			i++;
		}
		free(merged_cpblts);
	}

	return (retval);
}

int nc_client_handshake(struct nc_session *session, char** cpblts)
{
	nc_rpc *hello;
	int retval;

	/* just for sure, it should be already done */
	memset(session->session_id, '\0', SID_SIZE);

	/* create client's <hello> message */
	hello = nc_msg_client_hello(cpblts);
	if (hello == NULL) {
		return (EXIT_FAILURE);
	}

	retval = nc_handshake(session, cpblts, hello);
	nc_rpc_free(hello);

	return (retval);
}

int nc_server_handshake(struct nc_session *session, char** cpblts)
{
	nc_rpc *hello;
	int pid;
	int retval;

	/* set session ID == PID */
	pid = (int)getpid();
	if (snprintf(session->session_id, SID_SIZE, "%d", pid) <= 0) {
		ERROR("Unable to generate NETCONF session ID.");
		return (EXIT_FAILURE);
	}

	/* create server's <hello> message */
	hello = nc_msg_server_hello(cpblts, session->session_id);
	if (hello == NULL) {
		return (EXIT_FAILURE);
	}

	retval = nc_handshake(session, cpblts, hello);
	nc_rpc_free(hello);

	return (retval);
}

int check_hostkey(const char *host, const char* knownhosts_file, LIBSSH2_SESSION* ssh_session)
{
	int ret, knownhost_check, i;
	size_t len;
	const char *remotekey, *fingerprint_raw;
	char fingerprint_md5[48];
	int hostkey_type, hostkey_typebit;
	struct libssh2_knownhost *ssh_host;
	LIBSSH2_KNOWNHOSTS *knownhosts;

	/* MD5 hash size is 16B, SHA1 hash size is 20B */
	fingerprint_raw = libssh2_hostkey_hash(ssh_session, LIBSSH2_HOSTKEY_HASH_MD5);
	for (i = 0; i < 16; i++) {
		sprintf(&fingerprint_md5[i * 3], "%02x:", (uint8_t) fingerprint_raw[i]);
	}
	fingerprint_md5[47] = 0;

	knownhosts = libssh2_knownhost_init(ssh_session);
	if (knownhosts == NULL) {
		ERROR("Unable to init knownhost check.");
	} else {
		/* get host's fingerprint */
		remotekey = libssh2_session_hostkey(ssh_session, &len, &hostkey_type);
		if (remotekey == NULL && hostkey_type == LIBSSH2_HOSTKEY_TYPE_UNKNOWN) {
			ERROR("Unable to get host key.");
			libssh2_knownhost_free(knownhosts);
			return (EXIT_FAILURE);
		}
		hostkey_typebit = (hostkey_type == LIBSSH2_HOSTKEY_TYPE_RSA) ? LIBSSH2_KNOWNHOST_KEY_SSHRSA : LIBSSH2_KNOWNHOST_KEY_SSHDSS;

		/* get all hosts */
		if (knownhosts_file != NULL) {
			ret = libssh2_knownhost_readfile(knownhosts,
					knownhosts_file,
					LIBSSH2_KNOWNHOST_FILE_OPENSSH);
		} else {
			ret = 0;
		}
		if (ret < 0) {
			WARN("Unable to check against knownhost file.");
			if (callbacks.hostkey_check(host, hostkey_type, fingerprint_md5) == 0) {
				/* host authenticity authorized */
				libssh2_knownhost_free(knownhosts);
				return (EXIT_SUCCESS);
			} else {
				VERB("Host authenticity check negative.");
				libssh2_knownhost_free(knownhosts);
				return (EXIT_FAILURE);
			}
		} else {
			knownhost_check = libssh2_knownhost_check(knownhosts,
					host,
					remotekey,
					len,
					LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | hostkey_typebit,
					&ssh_host);

			DBG("Host check: %d, key: %s\n", knownhost_check,
					(knownhost_check <= LIBSSH2_KNOWNHOST_CHECK_MATCH) ? ssh_host->key : "<none>");

			switch (knownhost_check) {
			case LIBSSH2_KNOWNHOST_CHECK_MISMATCH:
				ERROR("Remote host %s identification changed!", host);
				libssh2_knownhost_free(knownhosts);
				return (EXIT_FAILURE);
			case LIBSSH2_KNOWNHOST_CHECK_FAILURE:
				ERROR("Knownhost checking failed.");
				libssh2_knownhost_free(knownhosts);
				return (EXIT_FAILURE);
			case LIBSSH2_KNOWNHOST_CHECK_MATCH:
				libssh2_knownhost_free(knownhosts);
				return (EXIT_SUCCESS);
			case LIBSSH2_KNOWNHOST_CHECK_NOTFOUND:
				if (callbacks.hostkey_check(host, hostkey_type, fingerprint_md5) == 1) {
					VERB("Host authenticity check negative.");
					libssh2_knownhost_free(knownhosts);
					return (EXIT_FAILURE);
				}
				/* authenticity authorized */
				break;
			}

			ret = libssh2_knownhost_add(knownhosts,
					host,
					NULL,
					remotekey,
					len,
					LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | hostkey_typebit,
					NULL);
			if (ret != 0) {
				WARN("Adding the known host %s failed!", host);
			} else {
				ret = libssh2_knownhost_writefile(knownhosts,
						knownhosts_file,
						LIBSSH2_KNOWNHOST_FILE_OPENSSH);
				if (ret) {
					WARN("Writing %s failed!", knownhosts_file);
				}
			}

			libssh2_knownhost_free(knownhosts);
			return (EXIT_SUCCESS);
		}

	}

	return (EXIT_FAILURE);
}

struct nc_session *nc_session_accept(struct nc_cpblts* capabilities)
{
	static int uniqueness = 0;
	struct nc_session *retval = NULL;
	struct nc_cpblts *server_cpblts = NULL;
	struct passwd *pw;

	/* check, that this function is called only once in the process */
	if (uniqueness != 0) {
		VERB("You are trying to accept multiple NETCONF sessions in the single SSH NETCONF Subsystem.");
		return (NULL);
	} else {
		uniqueness++;
	}

	/* allocate netconf session structure */
	retval = malloc(sizeof(struct nc_session));
	if (retval == NULL) {
		ERROR("Memory allocation failed (%s)", strerror(errno));
		uniqueness--;
		return (NULL);
	}
	memset(retval, 0, sizeof(struct nc_session));
	retval->libssh2_socket = -1;
	retval->fd_input = STDIN_FILENO;
	retval->fd_output = STDOUT_FILENO;

	/*
	 * get username - we are running as SSH Subsystem which was started
	 * under the user which was connecting to NETCONF server
	 */
	pw = getpwuid(geteuid());
	if (pw == NULL) {
		/* unable to get correct username */
		ERROR("Unable to set username for SSH connection (%s).", strerror(errno));
		nc_session_close(retval);
		uniqueness--;
		return (NULL);
	}
	retval->username = strdup(pw->pw_name);

	if (capabilities == NULL) {
		if ((server_cpblts = nc_session_get_cpblts_default()) == NULL) {
			VERB("Unable to set client's NETCONF capabilities.");
			nc_session_close(retval);
			uniqueness--;
			return (NULL);
		}
	} else {
		server_cpblts = capabilities;
	}

	if (nc_server_handshake(retval, server_cpblts->list) != 0) {
		nc_session_close(retval);
		uniqueness--;
		return (NULL);
	}

	if (capabilities == NULL) {
		nc_cpblts_free(server_cpblts);
	}

	return (retval);
}

struct nc_session *nc_session_connect(const char *host, unsigned short port, const char *username, struct nc_cpblts* cpblts)
{
	int i;
	int sock = -1;
	int auth = 0;
	struct addrinfo hints, *res_list, *res;
	struct passwd *pw;
	char *homedir, *knownhosts_file;
	char port_s[SHORT_INT_LENGTH];
	char *userauthlist;
	char *err_msg, *s;
	struct nc_cpblts *client_cpblts = NULL;
	struct nc_session *retval = NULL;

	/* set default values */
	if (host == NULL || strlen(host) == 0) {
		host = "localhost";
	}
	if (port == 0) {
		port = NC_PORT;
	}
	if (username == NULL || strlen(username) == 0) {
		pw = getpwuid(geteuid());
		if (pw == NULL) {
			/* unable to get correct username */
			ERROR("Unable to set username for SSH connection (%s).", strerror(errno));
			return (NULL);
		}
		username = pw->pw_name;
		homedir = pw->pw_dir;
	} else {
		pw = getpwnam(username);
		if (pw == NULL) {
			/* unable to get correct username */
			ERROR("Unable to set username for SSH connection (%s).", strerror(errno));
			return (NULL);
		}
		homedir = pw->pw_dir;
	}
	if (asprintf(&knownhosts_file, "%s/.ssh/known_hosts", homedir) == -1) {
		knownhosts_file = NULL;
	}
	if (access(knownhosts_file, R_OK | W_OK) == -1) {
		WARN("Unable to access known host file (%s).", knownhosts_file);
		free(knownhosts_file);
		knownhosts_file = NULL;
	}

	if (snprintf(port_s, SHORT_INT_LENGTH, "%d", port) < 0) {
		/* converting short int to the string failed */
	}

	/* Connect to SSH server */
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	i = getaddrinfo(host, port_s, &hints, &res_list);
	if (i != 0) {
		ERROR("Unable to translate host address (%s).", gai_strerror(i));
		return (NULL);
	}

	for (i = 0, res = res_list; res != NULL; res = res->ai_next) {
		sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		if (sock == -1) {
			/* socket was not created, try another resource */
			i = errno;
			continue;
		}

		if (connect(sock, res->ai_addr, res->ai_addrlen) == -1) {
			/* network connection failed, try another resource */
			i = errno;
			close(sock);
			sock = -1;
			continue;
		}

		/* we're done, network connection established */
		break;
	}
	freeaddrinfo(res_list);

	if (sock == -1) {
		ERROR("Unable to connect to the server (%s).", strerror(i));
		return (NULL);
	}

	/* allocate netconf session structure */
	retval = malloc(sizeof(struct nc_session));
	if (retval == NULL) {
		ERROR("Memory allocation failed (%s)", strerror(errno));
		return (NULL);
	}
	memset(retval, 0, sizeof(struct nc_session));
	retval->libssh2_socket = sock;
	retval->fd_input = -1;
	retval->fd_output = -1;
	retval->hostname = strdup(host);
	retval->username = strdup(username);
	retval->msgid = 1;

	/* Create a session instance */
	retval->ssh_session = libssh2_session_init();
	if (retval->ssh_session == NULL) {
		ERROR("Unable to initialize SSH session.");
		goto shutdown;
	}

	/*
	 * Set up the SSH session, deprecated variant is libssh2_session_startup()
	 */
	if (libssh2_session_handshake(retval->ssh_session, retval->libssh2_socket) != 0) {
		ERROR("Starting SSH session failed.");
		goto shutdown;
	}

	if (check_hostkey(host, knownhosts_file, retval->ssh_session) != 0) {
		goto shutdown;
	}

	/* check what authentication methods are available */
	userauthlist = libssh2_userauth_list(retval->ssh_session, username, strlen(username));
	if (userauthlist != NULL) {
		if (strstr(userauthlist, "password")) {
			if (callbacks.sshauth_password != NULL) {
				auth |= NC_SSH_AUTH_PASSWORD;
			}
		}
		if (strstr(userauthlist, "publickey")) {
			auth |= NC_SSH_AUTH_PUBLIC_KEYS;
		}
		if (strstr(userauthlist, "keyboard-interactive")) {
			if (callbacks.sshauth_interactive != NULL) {
				auth |= NC_SSH_AUTH_INTERACTIVE;
			}
		}
	}
	if ((auth == 0) && (libssh2_userauth_authenticated(retval->ssh_session) == 0)) {
		ERROR("Unable to authenticate to remote server (Authentication methods not supported).");
		goto shutdown;
	}

	/* select authentication according to preferences */
	for (i = 0; i < AUTH_COUNT; i++) {
		if ((sshauth_pref[i].type & auth) == 0) {
			continue;
		}

		if (sshauth_pref[i].value < 0) {
			/* all following auth methods are disabled via negative preference value */
			ERROR("Unable to authenticate to remote server (supported authentication method(s) are disabled).");
			goto shutdown;
		}

		/* found common authentication method */
		switch (sshauth_pref[i].type) {
		case NC_SSH_AUTH_PASSWORD:
			VERB("Password authentication (host %s, user %s)", host, username);
			s = callbacks.sshauth_password(username, host);
			if (libssh2_userauth_password(retval->ssh_session,
					username,
					s) != 0) {
				memset(s, 0, strlen(s));
				libssh2_session_last_error(retval->ssh_session, &err_msg, NULL, 0);
				ERROR("Authentication failed (%s)", err_msg);
				goto shutdown;
			}
			memset(s, 0, strlen(s));
			break;
		case NC_SSH_AUTH_INTERACTIVE:
			if (libssh2_userauth_keyboard_interactive(retval->ssh_session,
					username,
					callbacks.sshauth_interactive) != 0) {
				libssh2_session_last_error(retval->ssh_session, &err_msg, NULL, 0);
				ERROR("Authentication failed (%s)", err_msg);
				goto shutdown;
			}
			break;
		case NC_SSH_AUTH_PUBLIC_KEYS:
			s = callbacks.sshauth_passphrase(username, host,
			                callbacks.privatekey_filename != NULL ? callbacks.privatekey_filename : "~/.ssh/id_rsa");
			if (libssh2_userauth_publickey_fromfile(retval->ssh_session,
					username,
					callbacks.publickey_filename != NULL ? callbacks.publickey_filename : "~/.ssh/id_rsa.pub",
					callbacks.privatekey_filename != NULL ? callbacks.privatekey_filename : "~/.ssh/id_rsa",
					s) != 0) {
				memset(s, 0, strlen(s));
				libssh2_session_last_error(retval->ssh_session, &err_msg, NULL, 0);
				ERROR("Authentication failed (%s)", err_msg);
				goto shutdown;
			}
			memset(s, 0, strlen(s));
			break;
		}
		if (libssh2_userauth_authenticated(retval->ssh_session) == 1) {
			break;
		}
	}

	/* check a state of authentication */
	if (libssh2_userauth_authenticated(retval->ssh_session) == 0) {
		ERROR("Authentication failed.");
		goto shutdown;
	}

	/* open a channel */
	retval->ssh_channel = libssh2_channel_open_session(retval->ssh_session);
	if (retval->ssh_channel == NULL) {
		libssh2_session_last_error(retval->ssh_session, &err_msg, NULL, 0);
		ERROR("Opening SSH channel failed (%s)", err_msg);
		goto shutdown;
	}

	/* execute the NETCONF subsystem on the channel */
	if (libssh2_channel_subsystem(retval->ssh_channel, "netconf")) {
		libssh2_session_last_error(retval->ssh_session, &err_msg, NULL, 0);
		ERROR("Starting netconf SSH subsystem failed (%s)", err_msg);
		goto shutdown;
	}

	if (cpblts == NULL) {
		if ((client_cpblts = nc_session_get_cpblts_default()) == NULL) {
			VERB("Unable to set client's NETCONF capabilities.");
			goto shutdown;
		}
	} else {
		client_cpblts = cpblts;
	}

	if (nc_client_handshake(retval, client_cpblts->list) != 0) {
		goto shutdown;
	}

	if (cpblts == NULL) {
		nc_cpblts_free(client_cpblts);
	}

	if (knownhosts_file != NULL) {
		free(knownhosts_file);
	}

	return (retval);

shutdown:

	/* cleanup */
	nc_session_close(retval);

	if (knownhosts_file != NULL) {
		free(knownhosts_file);
	}

	if (cpblts == NULL && client_cpblts != NULL) {
		nc_cpblts_free(client_cpblts);
	}

	return (NULL);
}

