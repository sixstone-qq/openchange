/*
   OpenChange MAPI implementation testsuite

   Send mail to an Exchange server

   Copyright (C) Julien Kerihuel 2007
   Copyright (C) Fabien Le Mentec 2007
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/


#include <libmapi/libmapi.h>
#include <gen_ndr/ndr_exchange.h>
#include <param.h>
#include <credentials.h>
#include <torture/torture.h>
#include <torture/torture_proto.h>
#include <samba/popt.h>


#define CN_MSG_PROPS 3

BOOL torture_rpc_mapi_sendmail(struct torture_context *torture)
{
	enum MAPISTATUS		retval;
	TALLOC_CTX		*mem_ctx;
	BOOL			ret = True;
	const char		*profname;
	const char		*profdb;
	const char		*subject = lp_parm_string(-1, "mapi", "subject");
	const char		*body = lp_parm_string(-1, "mapi", "body");
	char			**usernames;
	char			**usernames_to;
	char			**usernames_cc;
	char			**usernames_bcc;
	uint32_t		index = 0;
	mapi_object_t		obj_message;
	mapi_object_t		obj_store;
	mapi_object_t		obj_outbox;
	mapi_id_t		id_outbox;
	struct mapi_session	*session;
	struct SRowSet		*SRowSet = NULL;
	struct FlagList		*flaglist = NULL;
	struct SPropTagArray	*SPropTagArray;
	struct SPropValue	SPropValue;
	struct SPropValue	props[CN_MSG_PROPS];
	uint32_t		msgflag;


	/* init torture */
	mem_ctx = talloc_init("torture_rpc_mapi_sendmail");

	/* init mapi */
	profdb = lp_parm_string(-1, "mapi", "profile_store");
	retval = MAPIInitialize(profdb);
	mapi_errstr("MAPIInitialize", GetLastError());
	if (retval != MAPI_E_SUCCESS) return False;

	/* default if null */
	if (subject == NULL) subject = "";
	if (body == NULL) body = "";

	/* profile name */
	profname = lp_parm_string(-1, "mapi", "profile");
	if (profname == 0) {
		DEBUG(0, ("Please specify a valid profile name\n"));
		return False;
	}

	retval = MapiLogonEx(&session, profname);
	mapi_errstr("MapiLogonEx", GetLastError());
	if (retval != MAPI_E_SUCCESS) return False;

	/* init objects */
	mapi_object_init(&obj_store);
	mapi_object_init(&obj_outbox);
	mapi_object_init(&obj_message);

	/* session::OpenMsgStore() */
	retval = OpenMsgStore(&obj_store);
	mapi_errstr("OpenMsgStore", GetLastError());
	if (retval != MAPI_E_SUCCESS) return False;

	/* id_outbox = store->GeOutboxFolder() */
	retval = GetOutboxFolder(&obj_store, &id_outbox);
	mapi_errstr("GetOutboxFolder", GetLastError());
	if (retval != MAPI_E_SUCCESS) return False;

	/* outbox = store->OpenFolder(id_outbox) */
	retval = OpenFolder(&obj_store, id_outbox, &obj_outbox);
	mapi_errstr("OpenFolder", GetLastError());
	if (retval != MAPI_E_SUCCESS) return False;

	/* message = outbox->CreateMessage() */
	retval = CreateMessage(&obj_outbox, &obj_message);
	mapi_errstr("CreateMessage", GetLastError());
	if (retval != MAPI_E_SUCCESS) return False;

	SPropTagArray = set_SPropTagArray(mem_ctx, 0x6,
					  PR_OBJECT_TYPE,
					  PR_DISPLAY_TYPE,
					  PR_7BIT_DISPLAY_NAME,
					  PR_DISPLAY_NAME,
					  PR_SMTP_ADDRESS,
					  PR_GIVEN_NAME);

	usernames_to = get_cmdline_recipients(mem_ctx, "to");
	usernames_cc = get_cmdline_recipients(mem_ctx, "cc");
	usernames_bcc = get_cmdline_recipients(mem_ctx, "bcc");
	usernames = collapse_recipients(mem_ctx, usernames_to, usernames_cc, usernames_bcc);

	/* ResolveNames */
	retval = ResolveNames((const char **)usernames, SPropTagArray, &SRowSet, &flaglist, 0);
	mapi_errstr("ResolveNames", GetLastError());
	if (retval != MAPI_E_SUCCESS) return False;
	
	if (!SRowSet) {
		printf("None of the recipients were resolved\n");
		return False;
	}
	
	set_usernames_RecipientType(&index, SRowSet, usernames_to, flaglist, MAPI_TO);
	set_usernames_RecipientType(&index, SRowSet, usernames_cc, flaglist, MAPI_CC);
	set_usernames_RecipientType(&index, SRowSet, usernames_bcc, flaglist, MAPI_BCC);

	SPropValue.ulPropTag = PR_SEND_INTERNET_ENCODING;
	SPropValue.value.l = 0;
	SRowSet_propcpy(mem_ctx, SRowSet, SPropValue);
	
	/* ModifyRecipients */
	retval = ModifyRecipients(&obj_message, SRowSet);
	mapi_errstr("ModifyRecipients", GetLastError());
	if (retval != MAPI_E_SUCCESS) return False;

	retval = MAPIFreeBuffer(SRowSet);
	mapi_errstr("MAPIFreeBuffer: SRowSet", GetLastError());

	retval = MAPIFreeBuffer(flaglist);
	mapi_errstr("MAPIFreeBuffer: flaglist", GetLastError());

	/* message->SetProps() */
	msgflag = MSGFLAG_UNSENT;
	set_SPropValue_proptag(&props[0], PR_SUBJECT, (void *)subject);
	set_SPropValue_proptag(&props[1], PR_BODY, (void *)body);
	set_SPropValue_proptag(&props[2], PR_MESSAGE_FLAGS, (void *)&msgflag);

	retval = SetProps(&obj_message, props, CN_MSG_PROPS);
	mapi_errstr("SetProps", GetLastError());
	if (retval != MAPI_E_SUCCESS) return False;

	/* message->SubmitMessage()
	 */
	retval = SubmitMessage(&obj_message);
	mapi_errstr("SubmitMessage", GetLastError());
	if (retval != MAPI_E_SUCCESS) return False;

	/* objects->Release()
	 */
	mapi_object_release(&obj_message);
	mapi_object_release(&obj_outbox);
	mapi_object_release(&obj_store);

	/* uninitialize mapi
	 */
	MAPIUninitialize();
	talloc_free(mem_ctx);

	return ret;
}
