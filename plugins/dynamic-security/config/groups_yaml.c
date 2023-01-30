/*
Copyright (c) 2020-2021 Roger Light <roger@atchoo.org>

All rights reserved. This program and the accompanying materials
are made available under the terms of the Eclipse Public License 2.0
and Eclipse Distribution License v1.0 which accompany this distribution.

The Eclipse Public License is available at
   https://www.eclipse.org/legal/epl-2.0/
and the Eclipse Distribution License is available at
  http://www.eclipse.org/org/documents/edl-v10.php.

SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause

Contributors:
   Roger Light - initial implementation and documentation.
   Akos Vandra-Meyer - addition of YAML file format
*/

#include "config.h"

#include <stdio.h>
#include <uthash.h>

#include "mosquitto.h"
#include "mosquitto_broker.h"
#include "json_help.h"

#include "dynamic_security.h"
#include "yaml.h"
#include "yaml_help.h"

int dynsec_groups__config_load_yaml(yaml_parser_t *parser, yaml_event_t *event, struct dynsec__data *data)
{
    struct dynsec__group *group;
    char *textname, *textdescription;

    struct dynsec__rolelist *rolelist;
    struct dynsec__clientlist *clientlist;

    int ret = MOSQ_ERR_SUCCESS;

    YAML_PARSER_SEQUENCE_FOR_ALL(parser, event, { goto error; }, {
            group = NULL;
            textname = textdescription = NULL;
            rolelist = NULL;
            clientlist = NULL;

            YAML_PARSER_MAPPING_FOR_ALL(parser, event, key, { goto error; }, {
                if (strcasecmp(key, "groupname") == 0) {
                    char *groupname;
                    YAML_EVENT_INTO_SCALAR_STRING(event, &groupname, { goto error; });
                    group = dynsec_groups__find(data, groupname);
                    if (!group) group = dynsec_groups__create(groupname); //TODO: Memory allocated for client is not freed if an error occurs later on.
                    if (!group) { ret = MOSQ_ERR_NOMEM; mosquitto_free(groupname); goto error; }

                    mosquitto_free(groupname);
                } else if (strcasecmp(key, "textname") == 0) {
                    YAML_EVENT_INTO_SCALAR_STRING(event, &textname, { goto error; });
                } else if (strcasecmp(key, "textdescription") == 0) {
                    YAML_EVENT_INTO_SCALAR_STRING(event, &textdescription, { goto error; });
                } else if (strcasecmp(key, "roles") == 0) {
                    if (dynsec_rolelist__load_from_yaml(parser, event, data, &rolelist)) goto error;
                } else if (strcasecmp(key, "clients") == 0) {
                    if (dynsec_clientlist__load_from_yaml(parser, event, data, &clientlist)) goto error;
                } else {
                    mosquitto_log_printf(MOSQ_LOG_ERR, "Unexpected key for group config %s \n", key);
                    yaml_dump_block(parser, event);
                }
            });

            if (group) {
                group->text_description = textdescription;
                group->text_name = textname;

                if (clientlist) {
                    struct dynsec__clientlist *iter;
                    struct dynsec__clientlist *tmp;

                    HASH_ITER(hh, clientlist, iter, tmp){
                        dynsec_clientlist__add(&group->clientlist, iter->client, iter->priority);
                        dynsec_grouplist__add(&iter->client->grouplist, group, iter->priority);
                        //Make sure not to clean up the actual role.
                        iter->client = NULL;
                    }
                    dynsec_clientlist__cleanup(&clientlist);
                }

                if (rolelist) {
                    struct dynsec__rolelist *iter;
                    struct dynsec__rolelist *tmp;

                    HASH_ITER(hh, rolelist, iter, tmp){

                        dynsec_rolelist__add(&group->rolelist, iter->role, iter->priority);
                        dynsec_grouplist__add(&iter->role->grouplist, group, iter->priority);
                        //Make sure not to clean up the actual role.
                        iter->role = NULL;
                    }
                    dynsec_rolelist__cleanup(&rolelist);
                }

                dynsec_groups__insert(data, group);
            } else {
                ret = MOSQ_ERR_INVAL;
                goto error;
           }
    });

    return MOSQ_ERR_SUCCESS;
error:
    mosquitto_free(textname);
    mosquitto_free(textdescription);
    dynsec_rolelist__cleanup(&rolelist);
    dynsec_clientlist__cleanup(&clientlist);
    return ret;
}

static int dynsec__config_add_groups_yaml(yaml_emitter_t *emitter, yaml_event_t *event, struct dynsec__data *data)
{
    struct dynsec__group *group, *group_tmp = NULL;

    yaml_sequence_start_event_initialize(event, NULL, (yaml_char_t *)YAML_SEQ_TAG,
                                         1, YAML_ANY_SEQUENCE_STYLE);
    if (!yaml_emitter_emit(emitter, event)) return MOSQ_ERR_UNKNOWN;

    HASH_ITER(hh, data->groups, group, group_tmp){
        yaml_mapping_start_event_initialize(event, NULL, (yaml_char_t *)YAML_MAP_TAG,
                                            1, YAML_ANY_MAPPING_STYLE);
        if (!yaml_emitter_emit(emitter, event)) return MOSQ_ERR_UNKNOWN;

        if (!yaml_emit_string_field(emitter, event, "groupname", group->groupname)) return MOSQ_ERR_UNKNOWN;
        if (group->text_name && !yaml_emit_string_field(emitter, event, "textname", group->text_name)) return MOSQ_ERR_UNKNOWN;
        if (group->text_description && !yaml_emit_string_field(emitter, event, "textdescription", group->text_description)) return MOSQ_ERR_UNKNOWN;


        yaml_scalar_event_initialize(event, NULL, (yaml_char_t *)YAML_STR_TAG,
                                     (yaml_char_t *)"roles", strlen("roles"), 1, 0, YAML_PLAIN_SCALAR_STYLE);
        if (!yaml_emitter_emit(emitter, event)) return MOSQ_ERR_UNKNOWN;
        if (dynsec_rolelist__all_to_yaml(group->rolelist, emitter, event)) return MOSQ_ERR_UNKNOWN;

        yaml_scalar_event_initialize(event, NULL, (yaml_char_t *)YAML_STR_TAG,
                                     (yaml_char_t *)"clients", strlen("clients"), 1, 0, YAML_PLAIN_SCALAR_STYLE);
        if (!yaml_emitter_emit(emitter, event)) return MOSQ_ERR_UNKNOWN;

        if (dynsec_clientlist__all_to_yaml(group->clientlist, emitter, event) != MOSQ_ERR_SUCCESS) return MOSQ_ERR_UNKNOWN;

        yaml_mapping_end_event_initialize(event);
        if (!yaml_emitter_emit(emitter, event)) return MOSQ_ERR_UNKNOWN;
    }

    yaml_sequence_end_event_initialize(event);
    if (!yaml_emitter_emit(emitter, event)) return MOSQ_ERR_UNKNOWN;

    return MOSQ_ERR_SUCCESS;
}

int dynsec_groups__config_save_yaml(yaml_emitter_t *emitter, yaml_event_t *event, struct dynsec__data *data)
{
    yaml_scalar_event_initialize(event, NULL, (yaml_char_t *)YAML_STR_TAG,
                                 (yaml_char_t *)"groups", strlen("groups"), 1, 0, YAML_PLAIN_SCALAR_STYLE);
    if (!yaml_emitter_emit(emitter, event)) return MOSQ_ERR_UNKNOWN;
    if (dynsec__config_add_groups_yaml(emitter, event, data)) return MOSQ_ERR_UNKNOWN;

    if (data->anonymous_group && !yaml_emit_string_field(emitter, event, "anonymousGroup", data->anonymous_group->groupname)) return MOSQ_ERR_UNKNOWN;

    return MOSQ_ERR_SUCCESS;
}

