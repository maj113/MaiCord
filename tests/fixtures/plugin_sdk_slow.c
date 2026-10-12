/* ABI-5 test plugin whose command sleeps past the host time budget. */
#include "maicord/sdk/maicord_plugin.h"

__declspec(dllimport) void __stdcall Sleep(unsigned long milliseconds);

#define OWNER 0x4D433453304C4F57ull

static unsigned long g_sleep_ms = 30;

static mc_status MC_CALL on_command(void* state, uint32_t bytes, const mc_command_invocation* call, mc_text_output* out) {
    (void)state; (void)bytes; (void)call;
    Sleep(g_sleep_ms);
    out->action = MC_CMD_DONE;
    out->bytes = 0;
    return MC_OK;
}

static mc_status MC_CALL start(const mc_host* host, void* state, uint32_t state_bytes) {
    const mc_cmd_v1* commands = 0;
    mc_command command;
    char* raw = (char*)&command;
    uint32_t i;
    (void)state; (void)state_bytes;
    if (host->get_interface(host->binding, host->generation, "mc.cmd", 1, (const void**)&commands) != MC_OK)
        return MC_E_UNSUPPORTED;
    for (i = 0; i < sizeof(command); ++i) raw[i] = 0;
    command.size = sizeof(command);
    command.command_id = 1;
    command.name.data = "slow";
    command.name.bytes = 4;
    if (commands->register_command(host->binding, host->generation, &command) != MC_OK) return MC_E_INVALID;
    return commands->set_handler(host->binding, host->generation, on_command);
}

static void MC_CALL stop(void* state, uint32_t state_bytes) {
    (void)state; (void)state_bytes;
}

MC_EXPORT mc_status MC_CALL maicord_plugin_query_v5(const mc_host_info* host, mc_plugin_definition* definition) {
    /* A newer plugin build: fields the host does not know follow the definition. */
    struct { mc_plugin_definition base; uint64_t future[2]; } local;
    char* raw = (char*)&local;
    uint32_t i;
    (void)host;
    for (i = 0; i < sizeof(local); ++i) raw[i] = 0;
    local.base.size = (uint32_t)sizeof(local);
    local.base.abi_version = MC_ABI_VERSION;
    local.base.owner_id = OWNER;
    local.base.permissions = MC_PERM_COMMANDS;
    local.base.state_bytes = 16;
    local.base.state_alignment = 8;
    local.base.start = start;
    local.base.stop = stop;
    local.future[0] = local.future[1] = ~0ull;
    return mc_definition_publish(definition, &local.base);
}

MC_EXPORT void maicord_plugin_sdk_slow_configure(unsigned long sleep_ms) {
    g_sleep_ms = sleep_ms;
}

int MC_CALL maicord_plugin_sdk_slow_entry(void* instance, unsigned long reason, void* reserved) {
    (void)instance; (void)reason; (void)reserved;
    return 1;
}
