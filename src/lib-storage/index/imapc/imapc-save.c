/* Copyright (c) 2011-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "str.h"
#include "istream.h"
#include "istream-crlf.h"
#include "ostream.h"
#include "imap-date.h"
#include "imap-util.h"
#include "index-mail.h"
#include "mail-copy.h"
#include "mailbox-list-private.h"
#include "imapc-msgmap.h"
#include "imapc-storage.h"
#include "imapc-sync.h"
#include "imapc-mail.h"

struct imapc_save_context {
	struct mail_save_context ctx;

	struct imapc_mailbox *mbox;
	struct mail_index_transaction *trans;

	int fd;
	char *temp_path;
	struct istream *input;

	uint32_t dest_uid_validity;
	ARRAY_TYPE(seq_range) dest_saved_uids;
	unsigned int save_count;

	bool failed:1;
	bool finished:1;
};

struct imapc_save_cmd_context {
	struct imapc_save_context *ctx;
	int ret;
};

#define IMAPC_SAVECTX(s)	container_of(s, struct imapc_save_context, ctx)

void imapc_transaction_save_rollback(struct mail_save_context *_ctx);

struct mail_save_context *
imapc_save_alloc(struct mailbox_transaction_context *t)
{
	struct imapc_mailbox *mbox = IMAPC_MAILBOX(t->box);
	struct imapc_save_context *ctx;

	i_assert((t->flags & MAILBOX_TRANSACTION_FLAG_EXTERNAL) != 0);

	if (t->save_ctx == NULL) {
		ctx = i_new(struct imapc_save_context, 1);
		ctx->ctx.transaction = t;
		ctx->mbox = mbox;
		ctx->trans = t->itrans;
		ctx->fd = -1;
		t->save_ctx = &ctx->ctx;
	}
	return t->save_ctx;
}

int imapc_save_begin(struct mail_save_context *_ctx, struct istream *input)
{
	struct imapc_save_context *ctx = IMAPC_SAVECTX(_ctx);
	const char *path;

	i_assert(ctx->fd == -1);

	if (imapc_storage_client_handle_auth_failure(ctx->mbox->storage->client))
		return -1;

	ctx->fd = imapc_client_create_temp_fd(ctx->mbox->storage->client->client,
					      &path);
	if (ctx->fd == -1) {
		mail_set_critical(_ctx->dest_mail,
				  "Couldn't create temp file %s", path);
		ctx->failed = TRUE;
		return -1;
	}
	/* we may not know the size of the input, or be sure that it contains
	   only CRLFs. so we'll always first write the mail to a temp file and
	   upload it from there to remote server. */
	ctx->finished = FALSE;
	ctx->temp_path = i_strdup(path);
	ctx->input = i_stream_create_crlf(input);
	_ctx->data.output = o_stream_create_fd_file(ctx->fd, 0, FALSE);
	o_stream_cork(_ctx->data.output);
	return 0;
}

int imapc_save_continue(struct mail_save_context *_ctx)
{
	struct imapc_save_context *ctx = IMAPC_SAVECTX(_ctx);

	if (ctx->failed)
		return -1;

	if (index_storage_save_continue(_ctx, ctx->input, NULL) < 0) {
		ctx->failed = TRUE;
		return -1;
	}
	return 0;
}

static void imapc_save_appenduid(struct imapc_save_context *ctx,
				 const struct imapc_command_reply *reply,
				 uint32_t *uid_r)
{
	const char *const *args;
	uint32_t uid_validity, dest_uid;

	*uid_r = 0;

	/* <uidvalidity> <dest uid-set> */
	args = t_strsplit(reply->resp_text_value, " ");
	if (str_array_length(args) != 2)
		return;

	if (str_to_uint32(args[0], &uid_validity) < 0)
		return;
	if (ctx->dest_uid_validity == 0)
		ctx->dest_uid_validity = uid_validity;
	else if (ctx->dest_uid_validity != uid_validity)
		return;

	if (str_to_uint32(args[1], &dest_uid) == 0) {
		seq_range_array_add_with_init(&ctx->dest_saved_uids,
					      32, dest_uid);
		*uid_r = dest_uid;
	}
}

static void
imapc_save_add_to_index(struct imapc_save_context *ctx, uint32_t uid)
{
	struct mail *_mail = ctx->ctx.dest_mail;
	struct index_mail *imail = INDEX_MAIL(_mail);
	uint32_t seq;

	/* we'll temporarily append messages and at commit time expunge
	   them all, since we can't guarantee that no one else has saved
	   messages to remote server during our transaction */
	mail_index_append(ctx->trans, uid, &seq);
	mail_set_seq_saving(_mail, seq);
	imail->data.no_caching = TRUE;
	imail->data.forced_no_caching = TRUE;

	if (ctx->fd != -1) {
		struct imapc_mail *imapc_mail = IMAPC_MAIL(_mail);
		imail->data.stream = i_stream_create_fd_autoclose(&ctx->fd, 0);
		imapc_mail->header_fetched = TRUE;
		imapc_mail->body_fetched = TRUE;
		imapc_mail_init_stream(imapc_mail);
	}

	ctx->save_count++;
}

static void imapc_save_callback(const struct imapc_command_reply *reply,
				void *context)
{
	struct imapc_save_cmd_context *ctx = context;
	uint32_t uid = 0;

	if (reply->state == IMAPC_COMMAND_STATE_OK) {
		if (reply->resp_text_key != NULL &&
		    strcasecmp(reply->resp_text_key, "APPENDUID") == 0)
			imapc_save_appenduid(ctx->ctx, reply, &uid);
		imapc_save_add_to_index(ctx->ctx, uid);
		ctx->ret = 0;
	} else if (imapc_storage_client_handle_auth_failure(ctx->ctx->mbox->storage->client)) {
		ctx->ret = -1;
	} else if (reply->state == IMAPC_COMMAND_STATE_NO) {
		imapc_copy_error_from_reply(ctx->ctx->mbox->storage,
					    MAIL_ERROR_PARAMS, reply);
		ctx->ret = -1;
	} else {
		mailbox_set_critical(&ctx->ctx->mbox->box,
			"imapc: APPEND failed: %s", reply->text_full);
		ctx->ret = -1;
	}
	imapc_client_stop(ctx->ctx->mbox->storage->client->client);
}

static void
imapc_save_noop_callback(const struct imapc_command_reply *reply ATTR_UNUSED,
			 void *context)
{
	struct imapc_save_cmd_context *ctx = context;

	/* we don't really care about the reply */
	ctx->ret = 0;
	imapc_client_stop(ctx->ctx->mbox->storage->client->client);
}

static void
imapc_append_keywords(string_t *str, struct mail_keywords *kw)
{
	const ARRAY_TYPE(keywords) *kw_arr;
	const char *kw_str;
	unsigned int i;

	kw_arr = mail_index_get_keywords(kw->index);
	for (i = 0; i < kw->count; i++) {
		kw_str = array_idx_elem(kw_arr, kw->idx[i]);
		if (str_len(str) > 1)
			str_append_c(str, ' ');
		str_append(str, kw_str);
	}
}

static int imapc_save_append(struct imapc_save_context *ctx)
{
	struct mail_save_context *_ctx = &ctx->ctx;
	struct mail_save_data *mdata = &_ctx->data;
	struct imapc_command *cmd;
	struct imapc_save_cmd_context sctx;
	struct istream *input;
	const char *flags = "", *internaldate = "";

	if (mdata->flags != 0 || mdata->keywords != NULL) {
		string_t *str = t_str_new(64);

		str_append(str, " (");
		imap_write_flags(str, mdata->flags & ENUM_NEGATE(MAIL_RECENT),
				 NULL);
		if (mdata->keywords != NULL)
			imapc_append_keywords(str, mdata->keywords);
		str_append_c(str, ')');
		flags = str_c(str);
	}
	if (mdata->received_date != (time_t)-1) {
		internaldate = t_strdup_printf(" \"%s\"",
			imap_to_datetime(mdata->received_date));
	}

	ctx->mbox->exists_received = FALSE;

	input = i_stream_create_fd(ctx->fd, IO_BLOCK_SIZE);
	sctx.ctx = ctx;
	sctx.ret = -2;
	cmd = imapc_client_cmd(ctx->mbox->storage->client->client,
			       imapc_save_callback, &sctx);
	imapc_command_sendf(cmd, "APPEND %s%1s%1s %p",
		imapc_mailbox_get_remote_name(ctx->mbox),
		flags, internaldate, input);
	i_stream_unref(&input);
	while (sctx.ret == -2)
		imapc_mailbox_run(ctx->mbox);

	if (sctx.ret == 0 && ctx->mbox->selected &&
	    !ctx->mbox->exists_received) {
		/* e.g. Courier doesn't send EXISTS reply before the tagged
		   APPEND reply. That isn't exactly required by the IMAP RFC,
		   but it makes the behavior better. See if NOOP finds
		   the mail. */
		sctx.ret = -2;
		cmd = imapc_client_cmd(ctx->mbox->storage->client->client,
				       imapc_save_noop_callback, &sctx);
		imapc_command_set_flags(cmd, IMAPC_COMMAND_FLAG_RETRIABLE);
		imapc_command_send(cmd, "NOOP");
		while (sctx.ret == -2)
			imapc_mailbox_run(ctx->mbox);
	}
	return sctx.ret;
}

int imapc_save_finish(struct mail_save_context *_ctx)
{
	struct imapc_save_context *ctx = IMAPC_SAVECTX(_ctx);
	struct mail_storage *storage = _ctx->transaction->box->storage;

	ctx->finished = TRUE;

	if (!ctx->failed) {
		if (o_stream_finish(_ctx->data.output) < 0) {
			if (!mail_storage_set_error_from_errno(storage)) {
				mail_set_critical(_ctx->dest_mail,
					"write(%s) failed: %s", ctx->temp_path,
					o_stream_get_error(_ctx->data.output));
			}
			ctx->failed = TRUE;
		}
	}

	if (!ctx->failed) {
		if (imapc_save_append(ctx) < 0)
			ctx->failed = TRUE;
	}

	o_stream_unref(&_ctx->data.output);
	i_stream_unref(&ctx->input);
	i_close_fd_path(&ctx->fd, ctx->temp_path);
	i_free(ctx->temp_path);
	index_save_context_free(_ctx);
	return ctx->failed ? -1 : 0;
}

void imapc_save_cancel(struct mail_save_context *_ctx)
{
	struct imapc_save_context *ctx = IMAPC_SAVECTX(_ctx);

	ctx->failed = TRUE;
	(void)imapc_save_finish(_ctx);
}

int imapc_transaction_save_commit_pre(struct mail_save_context *_ctx)
{
	struct imapc_save_context *ctx = IMAPC_SAVECTX(_ctx);
	struct mail_transaction_commit_changes *changes =
		_ctx->transaction->changes;
	uint32_t i, last_seq;

	i_assert(ctx->finished);

	/* expunge all added messages from index before commit */
	last_seq = mail_index_view_get_messages_count(_ctx->transaction->view);
	for (i = 0; i < ctx->save_count; i++)
		mail_index_expunge(ctx->trans, last_seq - i);

	if (array_is_created(&ctx->dest_saved_uids)) {
		changes->uid_validity = ctx->dest_uid_validity;
		array_append_array(&changes->saved_uids, &ctx->dest_saved_uids);
	}
	return 0;
}

void imapc_transaction_save_commit_post(struct mail_save_context *_ctx,
					struct mail_index_transaction_commit_result *result ATTR_UNUSED)
{
	imapc_transaction_save_rollback(_ctx);
}

void imapc_transaction_save_rollback(struct mail_save_context *_ctx)
{
	struct imapc_save_context *ctx = IMAPC_SAVECTX(_ctx);

	/* FIXME: if we really want to rollback, we should expunge messages
	   we already saved */

	if (!ctx->finished)
		imapc_save_cancel(_ctx);

	if (array_is_created(&ctx->dest_saved_uids))
		array_free(&ctx->dest_saved_uids);
	i_free(ctx);
}

static void imapc_save_copyuid(struct imapc_save_context *ctx,
			       const struct imapc_command_reply *reply,
			       uint32_t *uid_r)
{
	const char *const *args;
	uint32_t uid_validity, dest_uid;

	*uid_r = 0;

	/* <uidvalidity> <source uid-set> <dest uid-set> */
	args = t_strsplit(reply->resp_text_value, " ");
	if (str_array_length(args) != 3)
		return;

	if (str_to_uint32(args[0], &uid_validity) < 0)
		return;
	if (ctx->dest_uid_validity == 0)
		ctx->dest_uid_validity = uid_validity;
	else if (ctx->dest_uid_validity != uid_validity)
		return;

	if (str_to_uint32(args[2], &dest_uid) == 0) {
		seq_range_array_add_with_init(&ctx->dest_saved_uids,
					      32, dest_uid);
		*uid_r = dest_uid;
	}
}

static void imapc_copy_callback(const struct imapc_command_reply *reply,
				void *context)
{
	struct imapc_save_cmd_context *ctx = context;
	uint32_t uid = 0;

	if (reply->state == IMAPC_COMMAND_STATE_OK) {
		if (reply->resp_text_key != NULL &&
		    strcasecmp(reply->resp_text_key, "COPYUID") == 0)
			imapc_save_copyuid(ctx->ctx, reply, &uid);
		imapc_save_add_to_index(ctx->ctx, uid);
		ctx->ret = 0;
	} else if (reply->state == IMAPC_COMMAND_STATE_NO) {
		imapc_copy_error_from_reply(ctx->ctx->mbox->storage,
					    MAIL_ERROR_PARAMS, reply);
		ctx->ret = -1;
	} else {
		mailbox_set_critical(&ctx->ctx->mbox->box,
			"imapc: COPY failed: %s", reply->text_full);
		ctx->ret = -1;
	}
	imapc_client_stop(ctx->ctx->mbox->storage->client->client);
}

int imapc_copy(struct mail_save_context *_ctx, struct mail *mail)
{
	struct imapc_save_context *ctx = IMAPC_SAVECTX(_ctx);
	struct mailbox_transaction_context *_t = _ctx->transaction;
	struct imapc_mailbox *src_mbox;
	struct imapc_msgmap *src_msgmap;
	struct imapc_command *cmd;
	struct imapc_save_cmd_context sctx;
	uint32_t rseq;

	i_assert((_t->flags & MAILBOX_TRANSACTION_FLAG_EXTERNAL) != 0);

	if (_t->box->storage == mail->box->storage) {
		src_mbox = IMAPC_MAILBOX(mail->box);
		/* same server, we can use COPY for the mail */
		src_msgmap =
			imapc_client_mailbox_get_msgmap(src_mbox->client_box);
		if (mail->expunged ||
		    !imapc_msgmap_uid_to_rseq(src_msgmap, mail->uid, &rseq)) {
			mail_storage_set_error(mail->box->storage,
					       MAIL_ERROR_EXPUNGED,
					       "Some of the requested messages no longer exist.");
			ctx->finished = TRUE;
			index_save_context_free(_ctx);
			return -1;
		}
		/* Mail has not been expunged and can be copied. */
		sctx.ret = -2;
		sctx.ctx = ctx;
		cmd = imapc_client_mailbox_cmd(src_mbox->client_box,
					       imapc_copy_callback, &sctx);
		imapc_command_sendf(cmd, "UID COPY %u %s",
				    mail->uid, _t->box->name);
		while (sctx.ret == -2)
			imapc_mailbox_run(src_mbox);
		ctx->finished = TRUE;
		index_save_context_free(_ctx);
		return sctx.ret;
	}
	return mail_storage_copy(_ctx, mail);
}
