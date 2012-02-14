#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>

#include "imapfilter.h"
#include "session.h"
#include "buffer.h"


extern options opts;

buffer obuf;			/* Output buffer. */

static int tag = 0x1000;	/* Every IMAP command is prefixed with a
				 * unique [:alnum:] string. */


int send_request(session *ssn, const char *fmt,...);
int send_continuation(session *ssn, const char *data, size_t len);


#define TRY(F)								     \
	switch ((F)) {							     \
	case -1:							     \
		if (request_login(&ssn, NULL, NULL, NULL, NULL, NULL) != -1) \
			return STATUS_NONE;				     \
		else							     \
			return -1;					     \
		break;							     \
	case STATUS_BYE:						     \
		close_connection(ssn);					     \
		session_destroy(ssn);					     \
		return -1;						     \
		break;							     \
	}


/*
 * Sends to server data; a command.
 */
int
send_request(session *ssn, const char *fmt,...)
{
	int n;
	va_list args;
	int t = tag;

	if (ssn->socket == -1)
		return -1;

	buffer_reset(&obuf);
	obuf.len = snprintf(obuf.data, obuf.size + 1, "%04X ", tag);

	va_start(args, fmt);
	n = vsnprintf(obuf.data + obuf.len, obuf.size - obuf.len -
	    strlen("\r\n") + 1, fmt, args);
	if (n > (int)obuf.size) {
		buffer_check(&obuf, n);
		vsnprintf(obuf.data + obuf.len, obuf.size - obuf.len -
		    strlen("\r\n") + 1, fmt, args);
	}
	obuf.len = strlen(obuf.data);
	va_end(args);

	snprintf(obuf.data + obuf.len, obuf.size - obuf.len + 1, "\r\n");
	obuf.len = strlen(obuf.data);

	if (!strncasecmp(fmt, "LOGIN", strlen("LOGIN"))) {
		debug("sending command (%d):\n\n%.*s*\r\n\n", ssn->socket,
		    obuf.len - strlen(ssn->password) - strlen("\"\"\r\n"),
		    obuf.data);
		verbose("C (%d): %.*s*\r\n", ssn->socket, obuf.len -
		    strlen(ssn->password) - strlen("\"\"\r\n"),  obuf.data);
	} else {
		debug("sending command (%d):\n\n%s\n", ssn->socket, obuf.data);
		verbose("C (%d): %s", ssn->socket, obuf.data);
	}

	if (socket_write(ssn, obuf.data, obuf.len) == -1)
		return -1;

	if (tag == 0xFFFF)	/* Tag always between 0x1000 and 0xFFFF. */
		tag = 0x0FFF;
	tag++;

	return t;
}

/*
 * Sends a response to a command continuation request.
 */
int
send_continuation(session *ssn, const char *data, size_t len)
{

	if (ssn->socket == -1)
		return -1;

	if (socket_write(ssn, data, len) == -1 ||
	    socket_write(ssn, "\r\n", strlen("\r\n")) == -1)
		return -1;

	if (opts.debug) {
		unsigned int i;

		debug("sending continuation data (%d):\n\n", ssn->socket);
		
		for (i = 0; i < len; i++)
			debugc(data[i]);
		
		debug("\r\n\n");
			
	}

	return 0;
}


/*
 * Reset any inactivity autologout timer on the server.
 */
int
request_noop(session *ssn)
{
	int t, r;

	TRY(t = send_request(ssn, "NOOP"));
	TRY(r = response_generic(ssn, t));

	return r;
}


/*
 * Connect to the server, login to the IMAP server, get it's capabilities, get
 * the namespace of the mailboxes.
 */
int
request_login(session **ssnptr, const char *server, const char *port, const
    char *ssl, const char *user, const char *pass)
{
	int t, r = -1, rg = -1;
	session *ssn = *ssnptr;
	
	if (*ssnptr && (*ssnptr)->socket != -1)
		return STATUS_NONE;

	if (!*ssnptr) {
		ssn = *ssnptr = session_new();

		ssn->server = xstrdup(server);
		ssn->port = xstrdup(port);
		ssn->username = xstrdup(user);
		ssn->password = xstrdup(pass);

		if ((!strncasecmp(ssl, "tls1", 4) ||
		    !strncasecmp(ssl, "ssl3", 4) ||
		    !strncasecmp(ssl, "ssl2", 4)))
			ssn->ssl = xstrdup(ssl);
	}

	if (open_connection(ssn) == -1)
		goto fail;

	if ((rg = response_greeting(ssn)) == -1)
		goto fail;

	if (opts.debug) {
		t = send_request(ssn, "NOOP");
		if (response_generic(ssn, t) == -1)
			goto fail;
	}

	t = send_request(ssn, "CAPABILITY");
	if (response_capability(ssn, t) == -1)
		goto fail;

#ifndef NO_SSLTLS
	if (!ssl && ssn->capabilities & CAPABILITY_STARTTLS &&
	    get_option_boolean("starttls")) {
		t = send_request(ssn, "STARTTLS");
		switch (response_generic(ssn, t)) {
		case STATUS_OK:
			if (open_secure_connection(ssn) == -1)
				goto fail;
			t = send_request(ssn, "CAPABILITY");
			if (response_capability(ssn, t) == -1)
				goto fail;
			break;
		case -1:
			goto fail;
			break;
		}
	}
#endif

	if (rg != STATUS_PREAUTH) {
#ifndef NO_CRAMMD5
		if (ssn->capabilities & CAPABILITY_CRAMMD5 &&
		    get_option_boolean("crammd5")) {
			unsigned char *in, *out;
			if ((t = send_request(ssn, "AUTHENTICATE CRAM-MD5"))
			    == -1)
				goto fail;
			if (response_authenticate(ssn, t, &in) ==
			    STATUS_CONTINUE) {
				if ((out = auth_cram_md5(user, pass, in)) == NULL)
					goto fail;
				send_continuation(ssn, (char *)(out),
				    strlen((char *)(out)));
				xfree(out);
				if ((r = response_generic(ssn, t)) == -1)
					goto fail;
			} else
				goto fail;
		}
#endif
		if (r != STATUS_OK) {
			t = send_request(ssn, "LOGIN \"%s\" \"%s\"", ssn->username,
			    ssn->password);
			if ((r = response_generic(ssn, t)) == -1)
				goto fail;
		}

		if (r == STATUS_NO) {
			error("username %s or password rejected at %s\n",
			    ssn->username, ssn->server);
			goto fail;
		}
	} else {
		r = STATUS_PREAUTH;
	}

	t = send_request(ssn, "CAPABILITY");
	if (response_capability(ssn, t) == -1)
		goto fail;

	if (ssn->capabilities & CAPABILITY_NAMESPACE &&
	    get_option_boolean("namespace")) {
		t = send_request(ssn, "NAMESPACE");
		if (response_namespace(ssn, t) == -1)
			goto fail;
	}

	if (ssn->selected) {
		t = send_request(ssn, "SELECT \"%s\"", ssn->selected);
		if (response_select(ssn, t) == -1)
			goto fail;
	}

	return r;
fail:
	close_connection(ssn);
	session_destroy(ssn);

	return -1;
}


/*
 * Logout from the IMAP server and disconnect from the server.
 */
int
request_logout(session *ssn)
{
	int t, r;

	t = send_request(ssn, "LOGOUT");
	r = response_generic(ssn, t);

	close_connection(ssn);
	session_destroy(ssn);

	return r;
}


/*
 * Get mailbox's status.
 */
int
request_status(session *ssn, const char *mbox, unsigned int *exists, unsigned
    int *recent, unsigned int *unseen, unsigned int *uidnext)
{
	int t, r;
	const char *m;

	m = apply_namespace(mbox, ssn->ns.prefix, ssn->ns.delim);

	if (ssn->protocol == PROTOCOL_IMAP4REV1) {
		TRY(t = send_request(ssn,
		    "STATUS \"%s\" (MESSAGES RECENT UNSEEN UIDNEXT)", m));
		TRY(r = response_status(ssn, t, exists, recent, unseen, uidnext));
	} else {
		TRY(t = send_request(ssn, "EXAMINE \"%s\"", m));
		TRY(r = response_examine(ssn, t, exists, recent));
	}

	return r;
}


/*
 * Open mailbox in read-write mode.
 */
int
request_select(session *ssn,
    const char *mbox)
{
	int t, r;
	const char *m;

	m = apply_namespace(mbox, ssn->ns.prefix, ssn->ns.delim);

	TRY(t = send_request(ssn, "SELECT \"%s\"", m));
	TRY(r = response_select(ssn, t));

	if (r == STATUS_OK) {
		if (ssn && ssn->selected)
			xfree(ssn->selected);
		ssn->selected = xstrdup(m);
	}
	
	return r;
}


/*
 * Close examined/selected mailbox.
 */
int
request_close(session *ssn)
{
	int t, r;

	TRY(t = send_request(ssn, "CLOSE"));
	TRY(r = response_generic(ssn, t));

	if (r == STATUS_OK && ssn->selected) {
		xfree(ssn->selected);
		ssn->selected = NULL;
	}

	return r;
}


/*
 * Remove all messages marked for deletion from selected mailbox.
 */
int
request_expunge(session *ssn)
{
	int t, r;

	TRY(t = send_request(ssn, "EXPUNGE"));
	TRY(r = response_generic(ssn, t));

	return r;
}


/*
 * List available mailboxes.
 */
int
request_list(session *ssn, const char *refer, const char *name, char **mboxs,
    char **folders)
{
	int t, r;
	const char *n;

	n = apply_namespace(name, ssn->ns.prefix, ssn->ns.delim);
	
	TRY(t = send_request(ssn, "LIST \"%s\" \"%s\"", refer, n));
	TRY(r = response_list(ssn, t, mboxs, folders));

	return r;
}


/*
 * List subscribed mailboxes.
 */
int
request_lsub(session *ssn, const char *refer, const char *name, char **mboxs,
    char **folders)
{
	int t, r;
	const char *n;

	n = apply_namespace(name, ssn->ns.prefix, ssn->ns.delim);

	TRY(t = send_request(ssn, "LSUB \"%s\" \"%s\"", refer, n));
	TRY(r = response_list(ssn, t, mboxs, folders));

	return r;
}


/*
 * Search selected mailbox according to the supplied search criteria.
 */
int
request_search(session *ssn, const char *criteria, const char *charset, char
    **mesgs)
{
	int t, r;

	if (charset != NULL && *charset != '\0') {
		TRY(t = send_request(ssn, "UID SEARCH CHARSET \"%s\" %s", charset,
		    criteria));
	} else {
		TRY(t = send_request(ssn, "UID SEARCH %s", criteria));
	}
	TRY(r = response_search(ssn, t, mesgs));

	return r;
}


/*
 * Fetch the FLAGS, INTERNALDATE and RFC822.SIZE of the messages.
 */
int
request_fetchfast(session *ssn, const char *mesg, char **flags, char **date,
    char **size)
{
	int t, r;

	TRY(t = send_request(ssn, "UID FETCH %s FAST", mesg));
	TRY(r = response_fetchfast(ssn, t, flags, date, size));

	return r;
}


/*
 * Fetch the FLAGS of the messages.
 */
int
request_fetchflags(session *ssn, const char *mesg, char **flags)
{
	int t, r;

	TRY(t = send_request(ssn, "UID FETCH %s FLAGS", mesg));
	TRY(r = response_fetchflags(ssn, t, flags));

	return r;
}


/*
 * Fetch the INTERNALDATE of the messages.
 */
int
request_fetchdate(session *ssn, const char *mesg, char **date)
{
	int t, r;

	TRY(t = send_request(ssn, "UID FETCH %s INTERNALDATE", mesg));
	TRY(r = response_fetchdate(ssn, t, date));

	return r;
}
/*
 * Fetch the RFC822.SIZE of the messages.
 */
int
request_fetchsize(session *ssn, const char *mesg, char **size)
{
	int t, r;

	TRY(t = send_request(ssn, "UID FETCH %s RFC822.SIZE", mesg));
	TRY(r = response_fetchsize(ssn, t, size));

	return r;
}


/*
 * Fetch the body structure, ie. BODYSTRUCTURE, of the messages.
 */
int
request_fetchstructure(session *ssn, const char *mesg, char **structure)
{
	int t, r;

	TRY(t = send_request(ssn, "UID FETCH %s BODYSTRUCTURE", mesg));
	TRY(r = response_fetchstructure(ssn, t, structure));

	return r;
}


/*
 * Fetch the header, ie. BODY[HEADER], of the messages.
 */
int
request_fetchheader(session *ssn, const char *mesg, char **header, size_t *len)
{
	int t, r;

	TRY(t = send_request(ssn, "UID FETCH %s BODY.PEEK[HEADER]", mesg));
	TRY(r = response_fetchbody(ssn, t, header, len));

	return r;
}


/*
 * Fetch the text, ie. BODY[TEXT], of the messages.
 */
int
request_fetchtext(session *ssn, const char *mesg, char **text, size_t *len)
{
	int t, r;

	TRY(t = send_request(ssn, "UID FETCH %s BODY.PEEK[TEXT]", mesg));
	TRY(r = response_fetchbody(ssn, t, text, len));

	return r;
}


/*
 * Fetch the specified header fields, ie. BODY[HEADER.FIELDS (<fields>)], of
 * the messages.
 */
int
request_fetchfields(session *ssn, const char *mesg, const char *headerfields,
    char **fields, size_t *len)
{
	int t, r;

	{
		int n = strlen("BODY.PEEK[HEADER.FIELDS ()]") +
		    strlen(headerfields) + 1;
		char f[n];

		snprintf(f, n, "%s%s%s", "BODY.PEEK[HEADER.FIELDS (",
		    headerfields, ")]");
		TRY(t = send_request(ssn, "UID FETCH %s %s", mesg, f));
	}
	TRY(r = response_fetchbody(ssn, t, fields, len));

	return r;
}


/*
 * Fetch the specified message part, ie. BODY[<part>], of the
 * messages.
 */
int
request_fetchpart(session *ssn, const char *mesg, const char *part, char
    **bodypart, size_t *len)
{
	int t, r;

	{
		int n = strlen("BODY.PEEK[]") + strlen(part) + 1;
		char f[n];

		snprintf(f, n, "%s%s%s", "BODY.PEEK[", part, "]");
		TRY(t = send_request(ssn, "UID FETCH %s %s", mesg, f));
	}
	TRY(r = response_fetchbody(ssn, t, bodypart, len));

	return r;
}


/*
 * Add, remove or replace the specified flags of the messages.
 */
int
request_store(session *ssn, const char *mesg, const char *mode, const char
    *flags)
{
	int t, r;

	TRY(t = send_request(ssn, "UID STORE %s %sFLAGS.SILENT (%s)", mesg, 
	    (!strncasecmp(mode, "add", 3) ? "+" :
	    !strncasecmp(mode, "remove", 6) ? "-" : ""), flags));
	TRY(r = response_generic(ssn, t));

	if (xstrcasestr(flags, "\\Deleted") && get_option_boolean("expunge")) {
		TRY(t = send_request(ssn, "EXPUNGE"));
		TRY(response_generic(ssn, t));
	}

	return r;
}


/*
 * Copy the specified messages to another mailbox.
 */
int
request_copy(session *ssn, const char *mesg, const char *mbox)
{
	int t, r;
	const char *m;

	m = apply_namespace(mbox, ssn->ns.prefix, ssn->ns.delim);

	do {
		TRY(t = send_request(ssn, "UID COPY %s \"%s\"", mesg, m));
		TRY(r = response_generic(ssn, t));
		switch (r) {
		case STATUS_TRYCREATE:
			TRY(t = send_request(ssn, "CREATE \"%s\"", m));
			TRY(response_generic(ssn, t));

			if (get_option_boolean("subscribe")) {
				TRY(t = send_request(ssn, "SUBSCRIBE \"%s\"", m));
				TRY(response_generic(ssn, t));
			}
			break;
		case -1:
			return -1;
			break;
		}
	} while (r == STATUS_TRYCREATE);

	return r;
}


/*
 * Append supplied message to the specified mailbox.
 */
int
request_append(session *ssn, const char *mbox, const char *mesg, size_t
    mesglen, const char *flags, const char *date)
{
	int t, r;
	const char *m;

	m = apply_namespace(mbox, ssn->ns.prefix, ssn->ns.delim);

	do {
		TRY(t = send_request(ssn, "APPEND \"%s\"%s%s%s%s%s%s {%d}", m,
			(flags ? " (" : ""), (flags ? flags : ""),
			(flags ? ")" : ""), (date ? " \"" : ""),
			(date ? date : ""), (date ? "\"" : ""), mesglen));
		TRY(r = response_continuation(ssn));

		switch (r) {
		case STATUS_CONTINUE:
			TRY(send_continuation(ssn, mesg, mesglen)); 
			TRY(r = response_generic(ssn, t));
			break;
		case STATUS_TRYCREATE:
			TRY(t = send_request(ssn, "CREATE \"%s\"", m));
			TRY(response_generic(ssn, t));

			if (get_option_boolean("subscribe")) {
				TRY(t = send_request(ssn, "SUBSCRIBE \"%s\"", m));
				TRY(response_generic(ssn, t));
			}
			break;
		case -1:
			return -1;
			break;
		}
	} while (r == STATUS_TRYCREATE);

	return r;
}


/*
 * Create the specified mailbox.
 */
int
request_create(session *ssn, const char *mbox)
{
	int t, r;
	const char *m;

	m = apply_namespace(mbox, ssn->ns.prefix, ssn->ns.delim);

	TRY(t = send_request(ssn, "CREATE \"%s\"", m));
	TRY(r = response_generic(ssn, t));

	return r;
}


/*
 * Delete the specified mailbox.
 */
int
request_delete(session *ssn, const char *mbox)
{
	int t, r;
	const char *m;

	m = apply_namespace(mbox, ssn->ns.prefix, ssn->ns.delim);

	TRY(t = send_request(ssn, "DELETE \"%s\"", m));
	TRY(r = response_generic(ssn, t));

	return r;
}


/*
 * Rename a mailbox.
 */
int
request_rename(session *ssn, const char *oldmbox, const char *newmbox)
{
	int t, r;
	char *o, *n;

	o = xstrdup(apply_namespace(oldmbox, ssn->ns.prefix, ssn->ns.delim));
	n = xstrdup(apply_namespace(newmbox, ssn->ns.prefix, ssn->ns.delim));

	TRY(t = send_request(ssn, "RENAME \"%s\" \"%s\"", o, n));
	TRY(r = response_generic(ssn, t));

	return r;
}


/*
 * Subscribe the specified mailbox.
 */
int
request_subscribe(session *ssn, const char *mbox)
{
	int t, r;
	const char *m;

	m = apply_namespace(mbox, ssn->ns.prefix, ssn->ns.delim);

	TRY(t = send_request(ssn, "SUBSCRIBE \"%s\"", m));
	TRY(r = response_generic(ssn, t));

	return r;
}


/*
 * Unsubscribe the specified mailbox.
 */
int
request_unsubscribe(session *ssn, const char *mbox)
{
	int t, r;
	const char *m;

	m = apply_namespace(mbox, ssn->ns.prefix, ssn->ns.delim);

	TRY(t = send_request(ssn, "UNSUBSCRIBE \"%s\"", m));
	TRY(r = response_generic(ssn, t));

	return r;
}


int
request_idle(session *ssn)
{
	int t, r, ri;

	if (!(ssn->capabilities & CAPABILITY_IDLE))
		return -1;

	do {
		ri = 0;

		TRY(t = send_request(ssn, "IDLE"));
		TRY(r = response_continuation(ssn));
		if (r == STATUS_CONTINUE) {
			TRY(ri = response_idle(ssn, t));
			TRY(send_continuation(ssn, "DONE", strlen("DONE")));
			TRY(r = response_generic(ssn, t));
		}
	} while (ri == STATUS_TIMEOUT);

	return r;
}
