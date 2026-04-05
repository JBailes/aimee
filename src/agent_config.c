/* agent_config.c: config loading/saving, agent routing, role checking, auth resolution */
#include "aimee.h"
#include "agent_config.h"
#include "cJSON.h"
#include <sys/stat.h>
#include <unistd.h>

/* --- Config path --- */

const char *agent_config_path(void)
{
   static char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/agents.json", config_default_dir());
   return path;
}

/* --- Env var expansion --- */

void agent_expand_env(const char *src, char *dst, size_t dst_len)
{
   if (!src || !src[0])
   {
      dst[0] = '\0';
      return;
   }

   if (src[0] == '$')
   {
      const char *val = getenv(src + 1);
      if (val)
      {
         snprintf(dst, dst_len, "%s", val);
         return;
      }
   }

   snprintf(dst, dst_len, "%s", src);
}

/* --- Load/Save config (with mtime cache) --- */

static agent_config_t g_agent_config_cache;
static time_t g_agent_config_mtime;
static int g_agent_config_cached;

int agent_load_config(agent_config_t *cfg)
{
   memset(cfg, 0, sizeof(*cfg));

   const char *path = agent_config_path();

   /* Return cached config if mtime unchanged and caching enabled */
   if (!getenv("AIMEE_NO_CACHE"))
   {
      struct stat st;
      if (stat(path, &st) == 0 && g_agent_config_cached && st.st_mtime == g_agent_config_mtime)
      {
         memcpy(cfg, &g_agent_config_cache, sizeof(*cfg));
         return 0;
      }
   }

   FILE *f = fopen(path, "r");
   if (!f)
      return -1;

   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (sz <= 0 || sz > 1024 * 1024)
   {
      fclose(f);
      return -1;
   }

   char *data = malloc((size_t)sz + 1);
   if (!data)
   {
      fclose(f);
      return -1;
   }
   size_t nread = fread(data, 1, (size_t)sz, f);
   data[nread] = '\0';
   fclose(f);

   cJSON *root = cJSON_Parse(data);
   free(data);
   if (!root)
      return -1;

   /* Default agent */
   cJSON *def = cJSON_GetObjectItem(root, "default_agent");
   if (def && cJSON_IsString(def))
      snprintf(cfg->default_agent, MAX_AGENT_NAME, "%s", def->valuestring);

   /* Fallback chain */
   cJSON *fb = cJSON_GetObjectItem(root, "fallback_chain");
   if (fb && cJSON_IsArray(fb))
   {
      int n = cJSON_GetArraySize(fb);
      if (n > MAX_FALLBACK)
         n = MAX_FALLBACK;
      for (int i = 0; i < n; i++)
      {
         cJSON *item = cJSON_GetArrayItem(fb, i);
         if (cJSON_IsString(item))
            snprintf(cfg->fallback_chain[cfg->fallback_count++], MAX_AGENT_NAME, "%s",
                     item->valuestring);
      }
   }

   /* Agents array */
   cJSON *agents = cJSON_GetObjectItem(root, "agents");
   if (agents && cJSON_IsArray(agents))
   {
      int n = cJSON_GetArraySize(agents);
      if (n > MAX_AGENTS)
         n = MAX_AGENTS;

      for (int i = 0; i < n; i++)
      {
         cJSON *a = cJSON_GetArrayItem(agents, i);
         if (!cJSON_IsObject(a))
            continue;

         agent_t *ag = &cfg->agents[cfg->agent_count];
         memset(ag, 0, sizeof(*ag));

         cJSON *v;
         v = cJSON_GetObjectItem(a, "name");
         if (v && cJSON_IsString(v))
            snprintf(ag->name, MAX_AGENT_NAME, "%s", v->valuestring);

         v = cJSON_GetObjectItem(a, "endpoint");
         if (v && cJSON_IsString(v))
            snprintf(ag->endpoint, MAX_ENDPOINT_LEN, "%s", v->valuestring);

         v = cJSON_GetObjectItem(a, "model");
         if (v && cJSON_IsString(v))
            snprintf(ag->model, MAX_MODEL_LEN, "%s", v->valuestring);

         v = cJSON_GetObjectItem(a, "api_key");
         if (v && cJSON_IsString(v))
            agent_expand_env(v->valuestring, ag->api_key, MAX_API_KEY_LEN);

         v = cJSON_GetObjectItem(a, "auth_cmd");
         if (v && cJSON_IsString(v))
            snprintf(ag->auth_cmd, MAX_AUTH_CMD_LEN, "%s", v->valuestring);

         v = cJSON_GetObjectItem(a, "auth_type");
         if (v && cJSON_IsString(v))
            snprintf(ag->auth_type, sizeof(ag->auth_type), "%s", v->valuestring);
         if (!ag->auth_type[0])
            snprintf(ag->auth_type, sizeof(ag->auth_type), "%s", "bearer");

         v = cJSON_GetObjectItem(a, "provider");
         if (v && cJSON_IsString(v))
            snprintf(ag->provider, sizeof(ag->provider), "%s", v->valuestring);
         if (!ag->provider[0])
            snprintf(ag->provider, sizeof(ag->provider), "%s", "openai");

         v = cJSON_GetObjectItem(a, "cost_tier");
         if (v && cJSON_IsNumber(v))
            ag->cost_tier = v->valueint;

         v = cJSON_GetObjectItem(a, "max_tokens");
         ag->max_tokens = (v && cJSON_IsNumber(v)) ? v->valueint : AGENT_DEFAULT_MAX_TOKENS;

         v = cJSON_GetObjectItem(a, "timeout_ms");
         ag->timeout_ms = (v && cJSON_IsNumber(v)) ? v->valueint : AGENT_DEFAULT_TIMEOUT_MS;

         v = cJSON_GetObjectItem(a, "enabled");
         ag->enabled = (!v || !cJSON_IsBool(v)) ? 1 : cJSON_IsTrue(v);

         v = cJSON_GetObjectItem(a, "tools_enabled");
         ag->tools_enabled = (v && cJSON_IsBool(v)) ? cJSON_IsTrue(v) : 0;

         v = cJSON_GetObjectItem(a, "max_turns");
         ag->max_turns = (v && cJSON_IsNumber(v)) ? v->valueint : -1;

         v = cJSON_GetObjectItem(a, "max_parallel");
         ag->max_parallel = (v && cJSON_IsNumber(v)) ? v->valueint : AGENT_DEFAULT_MAX_PARALLEL;

         v = cJSON_GetObjectItem(a, "exec_system_prompt");
         if (v && cJSON_IsString(v))
            snprintf(ag->exec_system_prompt, MAX_EXEC_PROMPT_LEN, "%s", v->valuestring);

         v = cJSON_GetObjectItem(a, "extra_headers");
         if (v && cJSON_IsString(v))
            snprintf(ag->extra_headers, sizeof(ag->extra_headers), "%s", v->valuestring);

         v = cJSON_GetObjectItem(a, "fallback_model");
         if (v && cJSON_IsString(v))
            snprintf(ag->fallback_model, sizeof(ag->fallback_model), "%s", v->valuestring);

         cJSON *exec_roles = cJSON_GetObjectItem(a, "exec_roles");
         if (exec_roles && cJSON_IsArray(exec_roles))
         {
            int ern = cJSON_GetArraySize(exec_roles);
            if (ern > MAX_EXEC_ROLES)
               ern = MAX_EXEC_ROLES;
            for (int j = 0; j < ern; j++)
            {
               cJSON *er = cJSON_GetArrayItem(exec_roles, j);
               if (cJSON_IsString(er))
                  snprintf(ag->exec_roles[ag->exec_role_count++], 32, "%s", er->valuestring);
            }
         }

         cJSON *roles = cJSON_GetObjectItem(a, "roles");
         if (roles && cJSON_IsArray(roles))
         {
            int rn = cJSON_GetArraySize(roles);
            if (rn > MAX_AGENT_ROLES)
               rn = MAX_AGENT_ROLES;
            for (int j = 0; j < rn; j++)
            {
               cJSON *r = cJSON_GetArrayItem(roles, j);
               if (cJSON_IsString(r))
                  snprintf(ag->roles[ag->role_count++], 32, "%s", r->valuestring);
            }
         }

         cfg->agent_count++;
      }
   }

   /* Network config */
   cJSON *net = cJSON_GetObjectItem(root, "network");
   if (net && cJSON_IsObject(net))
   {
      agent_network_t *nw = &cfg->network;
      memset(nw, 0, sizeof(*nw));

      cJSON *v;
      v = cJSON_GetObjectItem(net, "ssh_entry");
      if (v && cJSON_IsString(v))
         snprintf(nw->ssh_entry, sizeof(nw->ssh_entry), "%s", v->valuestring);
      v = cJSON_GetObjectItem(net, "ssh_key");
      if (v && cJSON_IsString(v))
         snprintf(nw->ssh_key, sizeof(nw->ssh_key), "%s", v->valuestring);

      cJSON *hosts = cJSON_GetObjectItem(net, "hosts");
      if (hosts && cJSON_IsArray(hosts))
      {
         int hn = cJSON_GetArraySize(hosts);
         if (hn > AGENT_MAX_NET_HOSTS)
            hn = AGENT_MAX_NET_HOSTS;
         for (int i = 0; i < hn; i++)
         {
            cJSON *h = cJSON_GetArrayItem(hosts, i);
            if (!cJSON_IsObject(h))
               continue;
            agent_net_host_t *host = &nw->hosts[nw->host_count];
            v = cJSON_GetObjectItem(h, "name");
            if (v && cJSON_IsString(v))
               snprintf(host->name, sizeof(host->name), "%s", v->valuestring);
            v = cJSON_GetObjectItem(h, "ip");
            if (v && cJSON_IsString(v))
               snprintf(host->ip, sizeof(host->ip), "%s", v->valuestring);
            v = cJSON_GetObjectItem(h, "user");
            if (v && cJSON_IsString(v))
               snprintf(host->user, sizeof(host->user), "%s", v->valuestring);
            v = cJSON_GetObjectItem(h, "port");
            host->port = (v && cJSON_IsNumber(v)) ? v->valueint : 0;
            v = cJSON_GetObjectItem(h, "desc");
            if (v && cJSON_IsString(v))
               snprintf(host->desc, sizeof(host->desc), "%s", v->valuestring);
            v = cJSON_GetObjectItem(h, "tunnel");
            if (v && cJSON_IsString(v))
               snprintf(host->tunnel, sizeof(host->tunnel), "%s", v->valuestring);
            nw->host_count++;
         }
      }

      cJSON *networks = cJSON_GetObjectItem(net, "networks");
      if (networks && cJSON_IsArray(networks))
      {
         int nn = cJSON_GetArraySize(networks);
         if (nn > AGENT_MAX_NETWORKS)
            nn = AGENT_MAX_NETWORKS;
         for (int i = 0; i < nn; i++)
         {
            cJSON *n = cJSON_GetArrayItem(networks, i);
            if (!cJSON_IsObject(n))
               continue;
            agent_net_def_t *nd = &nw->networks[nw->network_count];
            v = cJSON_GetObjectItem(n, "name");
            if (v && cJSON_IsString(v))
               snprintf(nd->name, sizeof(nd->name), "%s", v->valuestring);
            v = cJSON_GetObjectItem(n, "cidr");
            if (v && cJSON_IsString(v))
               snprintf(nd->cidr, sizeof(nd->cidr), "%s", v->valuestring);
            v = cJSON_GetObjectItem(n, "desc");
            if (v && cJSON_IsString(v))
               snprintf(nd->desc, sizeof(nd->desc), "%s", v->valuestring);
            nw->network_count++;
         }
      }

      /* Tunnel config */
      cJSON *tunnels = cJSON_GetObjectItem(net, "tunnels");
      if (tunnels && cJSON_IsArray(tunnels))
      {
         int tn = cJSON_GetArraySize(tunnels);
         if (tn > AGENT_MAX_TUNNELS)
            tn = AGENT_MAX_TUNNELS;
         agent_tunnel_mgr_t *tmgr = &cfg->tunnel_mgr;
         for (int i = 0; i < tn; i++)
         {
            cJSON *t = cJSON_GetArrayItem(tunnels, i);
            if (!cJSON_IsObject(t))
               continue;
            agent_tunnel_t *tun = &tmgr->tunnels[tmgr->tunnel_count];
            memset(tun, 0, sizeof(*tun));
            v = cJSON_GetObjectItem(t, "name");
            if (v && cJSON_IsString(v))
               snprintf(tun->name, sizeof(tun->name), "%s", v->valuestring);
            v = cJSON_GetObjectItem(t, "relay_ssh");
            if (v && cJSON_IsString(v))
               snprintf(tun->relay_ssh, sizeof(tun->relay_ssh), "%s", v->valuestring);
            v = cJSON_GetObjectItem(t, "relay_key");
            if (v && cJSON_IsString(v))
               snprintf(tun->relay_key, sizeof(tun->relay_key), "%s", v->valuestring);
            v = cJSON_GetObjectItem(t, "target_host");
            if (v && cJSON_IsString(v))
               snprintf(tun->target_host, sizeof(tun->target_host), "%s", v->valuestring);
            v = cJSON_GetObjectItem(t, "target_port");
            tun->target_port = (v && cJSON_IsNumber(v)) ? v->valueint : 22;
            v = cJSON_GetObjectItem(t, "reconnect_delay");
            tun->reconnect_delay_s = (v && cJSON_IsNumber(v)) ? v->valueint : 5;
            v = cJSON_GetObjectItem(t, "max_reconnects");
            tun->max_reconnects = (v && cJSON_IsNumber(v)) ? v->valueint : 0;
            tmgr->tunnel_count++;
         }
         if (tmgr->tunnel_count > 0)
            nw->tunnel_mgr = tmgr;
      }
   }

   cJSON_Delete(root);

   /* Update mtime cache */
   {
      struct stat st;
      if (stat(path, &st) == 0)
      {
         memcpy(&g_agent_config_cache, cfg, sizeof(g_agent_config_cache));
         g_agent_config_mtime = st.st_mtime;
         g_agent_config_cached = 1;
      }
   }

   return 0;
}

int agent_save_config(const agent_config_t *cfg)
{
   cJSON *root = cJSON_CreateObject();

   if (cfg->default_agent[0])
      cJSON_AddStringToObject(root, "default_agent", cfg->default_agent);

   cJSON *fb = cJSON_CreateArray();
   for (int i = 0; i < cfg->fallback_count; i++)
      cJSON_AddItemToArray(fb, cJSON_CreateString(cfg->fallback_chain[i]));
   cJSON_AddItemToObject(root, "fallback_chain", fb);

   cJSON *agents = cJSON_CreateArray();
   for (int i = 0; i < cfg->agent_count; i++)
   {
      const agent_t *ag = &cfg->agents[i];
      cJSON *a = cJSON_CreateObject();

      cJSON_AddStringToObject(a, "name", ag->name);
      cJSON_AddStringToObject(a, "endpoint", ag->endpoint);
      cJSON_AddStringToObject(a, "model", ag->model);
      if (ag->api_key[0])
         cJSON_AddStringToObject(a, "api_key", ag->api_key);
      if (ag->auth_cmd[0])
         cJSON_AddStringToObject(a, "auth_cmd", ag->auth_cmd);
      if (strcmp(ag->auth_type, "bearer") != 0)
         cJSON_AddStringToObject(a, "auth_type", ag->auth_type);
      if (strcmp(ag->provider, "openai") != 0)
         cJSON_AddStringToObject(a, "provider", ag->provider);

      cJSON *roles = cJSON_CreateArray();
      for (int j = 0; j < ag->role_count; j++)
         cJSON_AddItemToArray(roles, cJSON_CreateString(ag->roles[j]));
      cJSON_AddItemToObject(a, "roles", roles);

      cJSON_AddNumberToObject(a, "cost_tier", ag->cost_tier);
      cJSON_AddNumberToObject(a, "max_tokens", ag->max_tokens);
      cJSON_AddNumberToObject(a, "timeout_ms", ag->timeout_ms);
      cJSON_AddBoolToObject(a, "enabled", ag->enabled);

      if (ag->tools_enabled)
      {
         cJSON_AddBoolToObject(a, "tools_enabled", ag->tools_enabled);
         cJSON_AddNumberToObject(a, "max_turns", ag->max_turns);
         if (ag->max_parallel != AGENT_DEFAULT_MAX_PARALLEL)
            cJSON_AddNumberToObject(a, "max_parallel", ag->max_parallel);
         if (ag->exec_system_prompt[0])
            cJSON_AddStringToObject(a, "exec_system_prompt", ag->exec_system_prompt);
         if (ag->exec_role_count > 0)
         {
            cJSON *er = cJSON_CreateArray();
            for (int j = 0; j < ag->exec_role_count; j++)
               cJSON_AddItemToArray(er, cJSON_CreateString(ag->exec_roles[j]));
            cJSON_AddItemToObject(a, "exec_roles", er);
         }
      }

      if (ag->extra_headers[0])
         cJSON_AddStringToObject(a, "extra_headers", ag->extra_headers);
      if (ag->fallback_model[0])
         cJSON_AddStringToObject(a, "fallback_model", ag->fallback_model);

      cJSON_AddItemToArray(agents, a);
   }
   cJSON_AddItemToObject(root, "agents", agents);

   /* Network config (only write if ssh_entry is set) */
   if (cfg->network.ssh_entry[0])
   {
      const agent_network_t *nw = &cfg->network;
      cJSON *net = cJSON_CreateObject();
      cJSON_AddStringToObject(net, "ssh_entry", nw->ssh_entry);
      if (nw->ssh_key[0])
         cJSON_AddStringToObject(net, "ssh_key", nw->ssh_key);

      if (nw->host_count > 0)
      {
         cJSON *hosts = cJSON_CreateArray();
         for (int i = 0; i < nw->host_count; i++)
         {
            const agent_net_host_t *h = &nw->hosts[i];
            cJSON *hobj = cJSON_CreateObject();
            cJSON_AddStringToObject(hobj, "name", h->name);
            cJSON_AddStringToObject(hobj, "ip", h->ip);
            if (h->user[0])
               cJSON_AddStringToObject(hobj, "user", h->user);
            if (h->port > 0)
               cJSON_AddNumberToObject(hobj, "port", h->port);
            if (h->desc[0])
               cJSON_AddStringToObject(hobj, "desc", h->desc);
            if (h->tunnel[0])
               cJSON_AddStringToObject(hobj, "tunnel", h->tunnel);
            cJSON_AddItemToArray(hosts, hobj);
         }
         cJSON_AddItemToObject(net, "hosts", hosts);
      }

      if (nw->network_count > 0)
      {
         cJSON *nets = cJSON_CreateArray();
         for (int i = 0; i < nw->network_count; i++)
         {
            const agent_net_def_t *nd = &nw->networks[i];
            cJSON *nobj = cJSON_CreateObject();
            cJSON_AddStringToObject(nobj, "name", nd->name);
            cJSON_AddStringToObject(nobj, "cidr", nd->cidr);
            if (nd->desc[0])
               cJSON_AddStringToObject(nobj, "desc", nd->desc);
            cJSON_AddItemToArray(nets, nobj);
         }
         cJSON_AddItemToObject(net, "networks", nets);
      }

      /* Tunnel config */
      if (cfg->tunnel_mgr.tunnel_count > 0)
      {
         cJSON *tarr = cJSON_CreateArray();
         for (int i = 0; i < cfg->tunnel_mgr.tunnel_count; i++)
         {
            const agent_tunnel_t *tun = &cfg->tunnel_mgr.tunnels[i];
            cJSON *tobj = cJSON_CreateObject();
            cJSON_AddStringToObject(tobj, "name", tun->name);
            cJSON_AddStringToObject(tobj, "relay_ssh", tun->relay_ssh);
            if (tun->relay_key[0])
               cJSON_AddStringToObject(tobj, "relay_key", tun->relay_key);
            cJSON_AddStringToObject(tobj, "target_host", tun->target_host);
            cJSON_AddNumberToObject(tobj, "target_port", tun->target_port);
            if (tun->reconnect_delay_s != 5)
               cJSON_AddNumberToObject(tobj, "reconnect_delay", tun->reconnect_delay_s);
            if (tun->max_reconnects > 0)
               cJSON_AddNumberToObject(tobj, "max_reconnects", tun->max_reconnects);
            cJSON_AddItemToArray(tarr, tobj);
         }
         cJSON_AddItemToObject(net, "tunnels", tarr);
      }

      cJSON_AddItemToObject(root, "network", net);
   }

   char *json = cJSON_Print(root);
   cJSON_Delete(root);
   if (!json)
      return -1;

   /* Ensure directory exists */
   char dir[MAX_PATH_LEN];
   snprintf(dir, sizeof(dir), "%s", config_default_dir());
   mkdir(dir, 0700);

   FILE *f = fopen(agent_config_path(), "w");
   if (!f)
   {
      free(json);
      return -1;
   }
   fputs(json, f);
   fputc('\n', f);
   fclose(f);
   /* Restrict permissions (contains API keys and auth tokens) */
   chmod(agent_config_path(), 0600);
   free(json);
   return 0;
}

/* --- Routing --- */

int agent_has_role(const agent_t *agent, const char *role)
{
   for (int i = 0; i < agent->role_count; i++)
   {
      if (strcmp(agent->roles[i], role) == 0)
         return 1;
   }
   return 0;
}

static int agent_supports_role(const agent_t *agent, const char *role)
{
   if (agent_has_role(agent, role))
      return 1;

   /* Execution roles can be handled by any enabled agent.  The tools_enabled
    * flag controls whether tool use is offered during execution, not whether
    * the agent is eligible for selection. */
   if (agent_is_exec_role(agent, role))
      return 1;

   return 0;
}

agent_t *agent_find(agent_config_t *cfg, const char *name)
{
   for (int i = 0; i < cfg->agent_count; i++)
   {
      if (strcmp(cfg->agents[i].name, name) == 0)
         return &cfg->agents[i];
   }
   return NULL;
}

agent_t *agent_route(agent_config_t *cfg, const char *role)
{
   /* Try default agent first */
   if (cfg->default_agent[0])
   {
      agent_t *def = agent_find(cfg, cfg->default_agent);
      if (def && def->enabled && agent_supports_role(def, role))
         return def;
   }

   /* Find cheapest enabled agent with the role */
   agent_t *best = NULL;
   for (int i = 0; i < cfg->agent_count; i++)
   {
      agent_t *ag = &cfg->agents[i];
      if (!ag->enabled || !agent_supports_role(ag, role))
         continue;
      if (!best || ag->cost_tier < best->cost_tier)
         best = ag;
   }
   return best;
}

/* --- Exec role check --- */

static const char *default_exec_roles[] = {"deploy",   "validate", "test",
                                           "diagnose", "execute",  "review"};
#define DEFAULT_EXEC_ROLE_COUNT 6

int agent_is_exec_role(const agent_t *agent, const char *role)
{
   if (agent->exec_role_count > 0)
   {
      for (int i = 0; i < agent->exec_role_count; i++)
      {
         if (strcmp(agent->exec_roles[i], role) == 0)
            return 1;
      }
      return 0;
   }
   for (int i = 0; i < DEFAULT_EXEC_ROLE_COUNT; i++)
   {
      if (strcmp(default_exec_roles[i], role) == 0)
         return 1;
   }
   return 0;
}

/* --- Auth resolution --- */

int agent_resolve_auth(const agent_t *agent, char *buf, size_t buf_len)
{
   buf[0] = '\0';

   if (strcmp(agent->auth_type, "none") == 0)
      return 0;

   if (strcmp(agent->auth_type, "oauth") == 0 && agent->auth_cmd[0])
   {
      /* Run auth_cmd via safe_exec_capture (no shell injection) */
      char *auth_tokens[32];
      int auth_tc = shlex_split(agent->auth_cmd, auth_tokens, 32);
      if (auth_tc <= 0)
         return -1;
      const char *auth_argv[33];
      for (int ai = 0; ai < auth_tc && ai < 32; ai++)
         auth_argv[ai] = auth_tokens[ai];
      auth_argv[auth_tc] = NULL;
      char *output = NULL;
      int status = safe_exec_capture(auth_argv, &output, MAX_API_KEY_LEN);
      for (int ai = 0; ai < auth_tc; ai++)
         free(auth_tokens[ai]);
      if (status != 0 || !output || !output[0])
      {
         free(output);
         return -1;
      }
      char token[MAX_API_KEY_LEN];
      snprintf(token, sizeof(token), "%s", output);
      free(output);
      /* Strip trailing newline */
      size_t len = strlen(token);
      while (len > 0 && (token[len - 1] == '\n' || token[len - 1] == '\r'))
         token[--len] = '\0';
      if (!token[0])
         return -1;

      snprintf(buf, buf_len, "Authorization: Bearer %s", token);
      return 0;
   }

   /* x-api-key auth (Anthropic): resolve via auth_cmd or api_key */
   if (strcmp(agent->auth_type, "x-api-key") == 0)
   {
      if (agent->auth_cmd[0])
      {
         char *auth_tokens[32];
         int auth_tc = shlex_split(agent->auth_cmd, auth_tokens, 32);
         if (auth_tc <= 0)
            return -1;
         const char *auth_argv[33];
         for (int ai = 0; ai < auth_tc && ai < 32; ai++)
            auth_argv[ai] = auth_tokens[ai];
         auth_argv[auth_tc] = NULL;
         char *output = NULL;
         int status = safe_exec_capture(auth_argv, &output, MAX_API_KEY_LEN);
         for (int ai = 0; ai < auth_tc; ai++)
            free(auth_tokens[ai]);
         if (status != 0 || !output || !output[0])
         {
            free(output);
            return -1;
         }
         char token[MAX_API_KEY_LEN];
         snprintf(token, sizeof(token), "%s", output);
         free(output);
         size_t len = strlen(token);
         while (len > 0 && (token[len - 1] == '\n' || token[len - 1] == '\r'))
            token[--len] = '\0';
         if (!token[0])
            return -1;
         snprintf(buf, buf_len, "x-api-key: %s", token);
         return 0;
      }
      if (agent->api_key[0])
      {
         snprintf(buf, buf_len, "x-api-key: %s", agent->api_key);
         return 0;
      }
   }

   /* Default: bearer token from api_key */
   if (agent->api_key[0])
   {
      snprintf(buf, buf_len, "Authorization: Bearer %s", agent->api_key);
      return 0;
   }

   return 0; /* no auth needed (e.g., local Ollama) */
}
