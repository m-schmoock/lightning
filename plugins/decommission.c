#include <ccan/array_size/array_size.h>
#include <ccan/json_out/json_out.h>
#include <ccan/tal/str/str.h>
#include <ccan/time/time.h>
#include <common/json_command.h>
#include <common/json_stream.h>
#include <common/utils.h>
#include <inttypes.h>
#include <plugins/libplugin.h>

enum COMMISSION_STATE {
	COMMISSIONED,
	DECOMMISSIONING,
	DECOMMISSIONED,
	NUM_DECOMMISSION_STATES
};

struct decomm_state {
	/* Global state */
	struct plugin *plugin;

	/* Current enum state */
	enum COMMISSION_STATE state;

	/* Funds redirect parameter */
	const char *address_or_xpub;

	/* close channel timeout in seconds */
	u64 timeout;
};

struct decomm_state ds;

static void disable_fund_channels(void)
{
}

static void disable_accept_channels(void)
{
}

static void send_available_funds(void)
{
	if (ds.address_or_xpub == NULL) return;
}


static void redirect_funds(void)
{
	if (ds.address_or_xpub == NULL) return;
}


static struct command_result *listpeers_done(struct command *cmd,
					     const char *buf,
					     const jsmntok_t *result,
					     void* ass)
{
	struct json_stream *ret;
	ret = jsonrpc_stream_success(cmd);
	json_add_string(ret, "foo", "bar");
	return command_finished(cmd, ret);
}

static struct command_result *close_all_channels(struct command *cmd)
{
	struct out_req *req;
	req = jsonrpc_request_start(cmd->plugin, cmd, "listpeers",
				    listpeers_done, forward_error, NULL);
	return send_outreq(cmd->plugin, req);
}

static struct command_result *json_decommission(struct command *cmd,
						const char *buffer,
						const jsmntok_t *params)
{
	u64 *timeout;
	if (!param(cmd, buffer, params,
		   p_opt("address_or_xpub", param_string, &ds.address_or_xpub),
		   p_opt_def("timeout", param_u64, &timeout, 60*60*24),
		   NULL))
		return command_param_failed();
	ds.timeout = *timeout;

	if (ds.state == DECOMMISSIONING)
		/* TODO: update params: timeout and address_or_xpub */
		return command_fail(cmd, PLUGIN_ERROR,
				    "Decommissioning already in progress");
	if (ds.state == DECOMMISSIONED)
		return command_fail(cmd, PLUGIN_ERROR,
				    "Node already decommissioned.");
	if (ds.state != COMMISSIONED)
		return command_fail(cmd, PLUGIN_ERROR, "FATAL: INVALID_STATE");

	/* TODO: do it */
	ds.state = DECOMMISSIONING;
	disable_fund_channels();
	disable_accept_channels();
	redirect_funds();
	send_available_funds();
	close_all_channels(cmd);

	if (ds.address_or_xpub) {
		return command_success_str(cmd, tal_fmt(tmpctx,
					   "Decommission started. "
					   "Timeout: %"PRIu64"s - "
					   "Redirecting funds to: %s",
					   ds.timeout, ds.address_or_xpub));
	} else {
		return command_success_str(cmd, tal_fmt(tmpctx,
					   "Decommission started. "
					   "Timeout: %"PRIu64"s",
					   ds.timeout));
	}
}

static struct command_result *json_recommission(struct command *cmd,
						const char *buffer,
						const jsmntok_t *params)
{
	if (!param(cmd, buffer, params, NULL))
		return command_param_failed();

	if (ds.state == COMMISSIONED)
		return command_fail(cmd, PLUGIN_ERROR, "Node still active.");
	if (ds.state != DECOMMISSIONING && ds.state != DECOMMISSIONED)
		return command_fail(cmd, PLUGIN_ERROR, "FATAL: INVALID_STATE");

	// TODO: do it
	ds.state = COMMISSIONED;
	return command_success_str(cmd, "Decommissioning cancelled. Good luck "
					"in getting inbound liquidity again.");
}

static struct command_result *json_commissionstate(struct command *cmd,
						   const char *buffer,
						   const jsmntok_t *params)
{
	if (!param(cmd, buffer, params, NULL))
		return command_param_failed();

	if (ds.state == COMMISSIONED)
		return command_success_str(cmd, "Node still active.");
	if (ds.state == DECOMMISSIONED)
		return command_success_str(cmd, "Node already decommissioned.");
	if (ds.state != DECOMMISSIONING)
		return command_fail(cmd, PLUGIN_ERROR, "FATAL: INVALID_STATE");

	return command_success_str(cmd, "Decommissioning in progress. "
					"TODO: ETA, channels, funds, ... ");
}

static void init(struct plugin *p,
		  const char *buf UNUSED, const jsmntok_t *config UNUSED)
{
	plugin_log(p, LOG_INFORM, "Plugin initialize");

	ds.plugin = p;
	ds.address_or_xpub = NULL;
	ds.timeout = 60*60*24;

	/* TODO: get DB variables: state, timeout and redirect */
	if (ds.state == DECOMMISSIONING) {
		if (ds.address_or_xpub) {
			plugin_log(p, LOG_INFORM, tal_fmt(tmpctx,
				   "### DECOMMISSION IN PROGRESS ### "
				   "Releasing funds to: %s",
				   ds.address_or_xpub));
		} else {
			plugin_log(p, LOG_INFORM,
				   "### DECOMMISSION IN PROGRESS ### "
				   "Not redirecting funds.");
		}
	} else {
		plugin_log(p, LOG_DBG, "Decommissioning not in progress.");
	}
}

static const struct plugin_command commands[] = {
	{
		"decommission",
		"utility",
		"Shuts down a node permanently by closing all channels and "
		"redirecting funds. ",
		"Close all responsive channels, unlilaterally force-close any "
		"offline or uncooperative channels. If {address_or_xpub} is "
		"given, redirect any funds to external address or wallet. This "
		"state persists and is meant to be final, however it can be "
		"cancelled by `recommission`. ",
		json_decommission
	},{
		"recommission",
		"utility",
		"Cancels an ongoing decommissioning.",
		"Decommissioning is meant to be final, however it can be "
		"cancelled. Future released or received funds will no longer "
		"be redirected. ",
		json_recommission
	},{
		"commissionstate",
		"utility",
		"Shows the state of an ongoing decommissioning process.",
		"The state of an ongoing decommissioning contains the numbers "
		"open and already closed channels, released funds and remaining "
		"funds, remaining timout, ...",
		json_commissionstate
	}
};

int main(int argc, char *argv[])
{
	setup_locale();
	plugin_main(argv, init, PLUGIN_RESTARTABLE, commands,
		    ARRAY_SIZE(commands), NULL, 0, NULL, 0, NULL);
}
