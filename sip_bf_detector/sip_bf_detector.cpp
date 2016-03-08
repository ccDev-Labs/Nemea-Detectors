/**
 * \file sip_bf_detector.cpp
 * \brief Module for detecting brute-force attacks on Session Initiation Protocol.
 * \author Tomas Jansky <janskto1@fit.cvut.cz>
 * \date 2016
 */
/*
 * Copyright (C) 2016 CESNET
 *
 * LICENSE TERMS
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
 * This software is provided ``as is'', and any express or implied
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

#include "sip_bf_detector.h"
#include "fields.h"

UR_FIELDS (
   ipaddr DST_IP,
   ipaddr SRC_IP,
   uint64 LINK_BIT_FIELD,
   uint8 PROTOCOL,
   time TIME_FIRST,
   uint16 SIP_MSG_TYPE,
   uint16 SIP_STATUS_CODE,
   string SIP_CSEQ,
   string SIP_CALLING_PARTY
)

trap_module_info_t *module_info = NULL;

#define MODULE_BASIC_INFO(BASIC) \
  BASIC("SIP Brute-Force Detector","Module for detecting brute-force attacks on Session Initiation Protocol.",1,1)

#define MODULE_PARAMS(PARAM) \
   PARAM('a', "alert_threshold", "Number of unsuccessful authentication attempts for considering this behaviour as an attack (20 by default).", required_argument, "uint64") \
   PARAM('c', "check_mem_int", "Number of seconds between the checks on ceased attacks (120 by default).", required_argument, "uint64") \
   PARAM('f', "free_mem_int", "Number of seconds after last action to consider attack as ceased (2400 by default).", required_argument, "uint64")

static int stop = 0;
int verbose;
uint64_t g_alert_threshold = DEFAULT_ALERT_THRESHOLD;
uint64_t g_check_mem_interval = CHECK_MEMORY_INTERVAL;
uint64_t g_free_mem_interval = FREE_MEMORY_INTERVAL;

TRAP_DEFAULT_SIGNAL_HANDLER(stop = 1)

int compare_user_name(void *a, void *b)
{
   return strncmp((const char *)a, (const char *)b, MAX_LENGTH_SIP_FROM);
}

int compare_ipv4(void *a, void *b)
{
   uint32_t *h1, *h2;
   h1 = (uint32_t*)a;
   h2 = (uint32_t*)b;
   if (*h1 == *h2) {
      return EQUAL;
   }
   else if (*h1 < *h2) {
      return LESS;
   }

   return MORE;
}

int compare_ipv6(void * a, void * b)
{
   int ret;
   ret = memcmp(a, b, IP_VERSION_6_BYTES);
   if (ret == 0) {
      return EQUAL;
   } else if (ret < 0) {
      return LESS;
   }

   return MORE;
}

bool free_ceased_attacks(void *server_tree, ur_time_t actual_time, int key_length)
{
   time_t time_actual = (time_t)actual_time;
   static time_t time_last_check = 0;

   if (abs(time_actual - time_last_check) > g_check_mem_interval) {
      int is_there_next;
      b_plus_tree_item *b_item;

      b_item = b_plus_tree_create_list_item(server_tree);
      if (!b_item) {
         fprintf(stderr, "Error: b_plus_tree_create_list_item returned NULL.\n");
         return false;
      }

      is_there_next = b_plus_tree_get_list(server_tree, b_item);
      while (is_there_next == 1) {
         attacked_server_t *server = (attacked_server_t*)(b_item->value);
         if (!server->free_unused_users(time_actual)) {
            VERBOSE("Error: failed to remove ceased attacks.\n")
            b_plus_tree_destroy_list_item(b_item);
            return false;
         }

         if (b_plus_tree_get_count_of_values(server->m_user_tree) == 0) {
            b_plus_tree_destroy(server->m_user_tree);
            free(server->m_ip_addr);
            is_there_next = b_plus_tree_delete_item_from_list(server_tree, b_item);
         } else {
            is_there_next = b_plus_tree_get_next_item_from_list(server_tree, b_item);
         }
      }

      b_plus_tree_destroy_list_item(b_item);
      time_last_check = time_actual;  
   }

   return true;
}

bool generate_alert(const attacked_server_t *server, const attacked_user_t *user)
{
   char *s = NULL;
   json_t *root = json_object();
   json_t *attackers_arr = json_array();
   int breached = user->m_breached ? 1 : 0;
   const char *breacher = user->m_breacher ? user->m_breacher : "";
   char time_first[32];
   char time_breach[32];
   char time_last[32];
   const time_t tf = user->m_first_action;
   const time_t tl = user->m_last_action;
   const time_t tb = user->m_breached ? user->m_breach_time : 0;

   strftime(time_first, 31, "%F %T", gmtime(&tf));
   strftime(time_last, 31, "%F %T", gmtime(&tl));
   strftime(time_breach, 31, "%F %T", gmtime(&tb));
   time_first[31] = time_last[31] = time_breach[31] = '\0';

   json_object_set_new(root, "TargetIP", json_string(server->m_ip_addr));
   json_object_set_new(root, "SIPTo", json_string(user->m_user_name));
   json_object_set_new(root, "AttemptCount", json_integer(user->m_attack_total_count));
   json_object_set_new(root, "EventTime", json_string(time_first));
   json_object_set_new(root, "CeaseTime", json_string(time_last));
   json_object_set_new(root, "Breach", json_integer(breached));
   json_object_set_new(root, "BreacherIP", json_string(breacher));
   json_object_set_new(root, "BreachTime", json_string(time_breach));
   json_object_set_new(root, "Sources", attackers_arr);

   int is_there_next;
   b_plus_tree_item *b_item;

   b_item = b_plus_tree_create_list_item(user->m_attackers_tree);
   if (!b_item) {
      fprintf(stderr, "Error: b_plus_tree_create_list_item returned NULL.\n");
      return false;
   }

   is_there_next = b_plus_tree_get_list(user->m_attackers_tree, b_item);
   while (is_there_next == 1) {
      attacker_t *attacker = (attacker_t*)(b_item->value);
      json_t *attacker_json = json_object();
      char attack_start[32];
      const time_t atf = attacker->m_start;

      strftime(attack_start, 31, "%F %T", gmtime(&atf));
      attack_start[31] = '\0';

      json_object_set_new(attacker_json, "SourceIP", json_string(attacker->m_ip_addr));
      json_object_set_new(attacker_json, "AttemptCount", json_integer(attacker->m_count));
      json_object_set_new(attacker_json, "EventTime", json_string(time_first));

      json_array_append(attackers_arr, attacker_json);
      json_decref(attacker_json);
      is_there_next = b_plus_tree_get_next_item_from_list(user->m_attackers_tree, b_item);
   }

   b_plus_tree_destroy_list_item(b_item);

   s = json_dumps(root, 0);
   json_decref(root);
  
   int ret = trap_send(0, s, strlen(s));
   free(s);
   TRAP_DEFAULT_SEND_ERROR_HANDLING(ret, return true, return false);
   return true;
}

/* ***************  attacker_t  *************** */
bool attacker_t::initialize(ip_addr_t *ip_addr, ur_time_t start_time)
{
   m_ip_addr = (char*)malloc(INET6_ADDRSTRLEN + 1);
   if (!m_ip_addr) {
      fprintf(stderr, "Error: malloc failed.\n");
      return false;
   }

   ip_to_str(ip_addr,m_ip_addr);
   m_ip_addr[INET6_ADDRSTRLEN] = '\0';
   m_count = 0;
   m_start = start_time;
   return true;
}

void attacker_t::destroy()
{
   free(m_ip_addr);
}

/* ***************  attacked_user_t  *************** */

bool attacked_user_t::initialize(const sip_dataholder_t *sip_data)
{
   m_ipv4 = sip_data->ipv4;
   m_user_name = (char*)calloc(MAX_LENGTH_SIP_FROM, sizeof(char));
   if (!m_user_name) {
      fprintf(stderr, "Error: calloc failed.\n");
      return false;
   }

   memcpy(m_user_name, sip_data->sip_from, sip_data->sip_from_len);
   m_attackers_tree = b_plus_tree_initialize(5, sip_data->comp_func, sizeof(attacker_t), sip_data->tree_key_length);
   if (!m_attackers_tree) {
      fprintf(stderr, "Error: b_plus_tree_initialize returned NULL.\n");
      return false;
   }

   m_breached = false;
   m_breacher = NULL;
   m_first_action = m_last_action = sip_data->time_stamp;
   m_breach_time = 0;
   return true;
}

int attacked_user_t::add_attack(const sip_dataholder_t *sip_data, attacked_server_t *server)
{
   void *tree_key;
   uint32_t dst_ip;

   if (m_ipv4) {
      dst_ip = ip_get_v4_as_int(sip_data->ip_dst);
      tree_key = &dst_ip;
   } else {
      tree_key = sip_data->ip_dst;
   }   

   attacker_t *attacker = (attacker_t*)b_plus_tree_search(m_attackers_tree, tree_key);

   if (!attacker) {
      if (sip_data->status_code == SIP_STATUS_OK)
         return 0;

      attacker = (attacker_t*)b_plus_tree_insert_item(m_attackers_tree, tree_key);
      if (!attacker) {
         char ip_str[INET6_ADDRSTRLEN + 1];
         ip_to_str(sip_data->ip_dst, ip_str);
         ip_str[INET6_ADDRSTRLEN] = '\0';
         VERBOSE("Warning: unable to insert IP: %s to b+ tree.\n", ip_str)
         return -1;
      } else {
         if(!attacker->initialize(sip_data->ip_dst, sip_data->time_stamp)) {
            VERBOSE("Warning: attacker initialization failed.\n")
            return -1;           
         }
      }
   }

   if (sip_data->status_code == SIP_STATUS_OK) {
      if (m_breached)
         return 0;

      m_breached = true;
      m_breacher = (char*)malloc(INET6_ADDRSTRLEN + 1);
      if (!m_breacher) {
         fprintf(stderr, "Error: malloc failed.\n");
         return -1;
      }      
      ip_to_str(sip_data->ip_dst, m_breacher);
      m_breacher[INET6_ADDRSTRLEN] = '\0'; 
      m_breach_time = sip_data->time_stamp;
      if (m_attack_total_count >= g_alert_threshold) {
         if (!generate_alert(server, this)) {
            VERBOSE("Error: generate_alert failed.\n")
            return -1;
         }
      }

   } else {
      attacker->m_count++;
      m_attack_total_count++;
      if (m_attack_total_count >= g_alert_threshold && m_reported == false) {
         m_reported = true;
         if (!generate_alert(server, this)) {
            VERBOSE("Error: generate_alert failed.\n")
            return -1;
         }
      }
   }

   if (m_last_action < sip_data->time_stamp)
      m_last_action = sip_data->time_stamp;

   return 0;
}

bool attacked_user_t::destroy()
{
   int is_there_next;
   b_plus_tree_item *b_item;

   b_item = b_plus_tree_create_list_item(m_attackers_tree);
   if (!b_item) {
      fprintf(stderr, "Error: b_plus_tree_create_list_item returned NULL.\n");
      return false;
   }

   is_there_next = b_plus_tree_get_list(m_attackers_tree, b_item);
   while (is_there_next == 1) {
      attacker_t *attacker = (attacker_t*)(b_item->value);
      attacker->destroy();
      is_there_next = b_plus_tree_get_next_item_from_list(m_attackers_tree, b_item);
   }

   b_plus_tree_destroy_list_item(b_item);
   b_plus_tree_destroy(m_attackers_tree);
   free(m_user_name);
   free(m_breacher);
   return true;
}

/* ***************  attacked_server_t  *************** */

bool attacked_server_t::initialize(ip_addr_t *ip_addr)
{
   m_ip_addr = (char*)malloc(INET6_ADDRSTRLEN + 1);
   if (!m_ip_addr) {
      fprintf(stderr, "Error: malloc failed.\n");
      return false;
   }

   ip_to_str(ip_addr,m_ip_addr);
   m_ip_addr[INET6_ADDRSTRLEN] = '\0';
   m_user_tree = b_plus_tree_initialize(5, &compare_user_name, sizeof(attacked_user_t), MAX_LENGTH_SIP_FROM);
   if (!m_user_tree) {
      fprintf(stderr, "Error: b_plus_tree_initialize returned NULL.\n");
      return false;
   }

   return true;
}

bool attacked_server_t::free_unused_users(time_t time_actual)
{
   int is_there_next;
   b_plus_tree_item *b_item;

   b_item = b_plus_tree_create_list_item(m_user_tree);
   if (!b_item) {
      fprintf(stderr, "Error: b_plus_tree_create_list_item returned NULL.\n");
      return false;
   }

   is_there_next = b_plus_tree_get_list(m_user_tree, b_item);
   while (is_there_next == 1) {
      attacked_user_t *user = (attacked_user_t*)(b_item->value);
      if (abs(time_actual - user->m_last_action) > g_free_mem_interval) {
         if (user->m_attack_total_count >= g_alert_threshold) {
            if (!generate_alert(this, user)) {
               VERBOSE("Error: generate_alert failed.\n")
               b_plus_tree_destroy_list_item(b_item);
               return false;
            }
         }

         if (!user->destroy()) {
            VERBOSE("Error: failed to remove user from b+ tree.\n")
            b_plus_tree_destroy_list_item(b_item);
            return false;
         }

         is_there_next = b_plus_tree_delete_item_from_list(m_user_tree, b_item);
      } else {
         is_there_next = b_plus_tree_get_next_item_from_list(m_user_tree, b_item);
      }
   }

   b_plus_tree_destroy_list_item(b_item);
   return true;
}

bool attacked_server_t::destroy()
{
   int is_there_next;
   b_plus_tree_item *b_item;

   b_item = b_plus_tree_create_list_item(m_user_tree);
   if (!b_item) {
      fprintf(stderr, "Error: b_plus_tree_create_list_item returned NULL.\n");
      return false;
   }

   is_there_next = b_plus_tree_get_list(m_user_tree, b_item);
   while (is_there_next == 1) {
      attacked_user_t *user = (attacked_user_t*)(b_item->value);
      if (user->m_attack_total_count >= g_alert_threshold) {
         if (!generate_alert(this, user)) {
            VERBOSE("Error: generate_alert failed.\n")
            b_plus_tree_destroy_list_item(b_item);
            return false;
         }
      }

      if (!user->destroy()) {
         VERBOSE("Error: failed to remove user from b+ tree.\n")
         b_plus_tree_destroy_list_item(b_item);
         return false;
      }

      is_there_next = b_plus_tree_get_next_item_from_list(m_user_tree, b_item);
   }

   b_plus_tree_destroy_list_item(b_item);
   b_plus_tree_destroy(m_user_tree);
   free(m_ip_addr);
   return true;
}

int insert_attack_attempt(const sip_dataholder_t *sip_data)
{
   void *tree_key;
   uint32_t src_ip;

   if (sip_data->ipv4) {
      src_ip = ip_get_v4_as_int(sip_data->ip_src);
      tree_key = &src_ip;
   } else {
      tree_key = sip_data->ip_src;
   }   

   attacked_server_t *server = (attacked_server_t*)b_plus_tree_search(sip_data->tree, tree_key);
   if (!server) {
      if (sip_data->status_code == SIP_STATUS_OK)
         return 0;

      server = (attacked_server_t*)b_plus_tree_insert_item(sip_data->tree, tree_key);
      if (!server) {
         char ip_str[INET6_ADDRSTRLEN + 1];
         ip_to_str(sip_data->ip_src, ip_str);
         ip_str[INET6_ADDRSTRLEN] = '\0';
         VERBOSE("Error: unable to insert IP: %s to b+ tree.\n", ip_str)
         return -1;
      } else {
         if(!server->initialize(sip_data->ip_src)) {
            VERBOSE("Error: server initialization failed.\n")
            return -1;
         }
      }
   }

   char *user_name_tmp = (char*)calloc(MAX_LENGTH_SIP_FROM, sizeof(char));
   if (!user_name_tmp) {
      fprintf(stderr, "Error: calloc failed.\n");
      return -1;
   }

   memcpy(user_name_tmp, sip_data->sip_from, sip_data->sip_from_len);
   attacked_user_t *user = (attacked_user_t*)b_plus_tree_search(server->m_user_tree, user_name_tmp);
   if (!user) {
      if (sip_data->status_code == SIP_STATUS_OK) {
         free(user_name_tmp);
         return 0;
      }

      user = (attacked_user_t*)b_plus_tree_insert_item(server->m_user_tree, user_name_tmp);
      if (!user) {
         VERBOSE("Warning: unable to insert user: %s to b+ tree.\n", user_name_tmp)
         free(user_name_tmp);
         return -1;
      } else {
         if(!user->initialize(sip_data)) {
            VERBOSE("Error: user initialization failed.\n")
            return -1;
         }
      }
   }

   free(user_name_tmp);
   if (user->add_attack(sip_data, server)) {
      VERBOSE("Error: failed to add attack attempt for user.\n")
      return -1;
   }

   return 0;
}

void destroy_tree(void *tree)
{
   int is_there_next;
   b_plus_tree_item *b_item;

   b_item = b_plus_tree_create_list_item(tree);
   is_there_next = b_plus_tree_get_list(tree, b_item);
   while (is_there_next == 1) {
      attacked_server_t *server = (attacked_server_t*)(b_item->value);
      server->destroy();
      is_there_next = b_plus_tree_get_next_item_from_list(tree, b_item);
   }

   b_plus_tree_destroy_list_item(b_item);
   b_plus_tree_destroy(tree);
}

// Cut first 4 chars ("sip:") or 5 chars ("sips:") from input string and ignore ';' or '?' + string after it

int cut_sip_identifier(char **output_str, char *input_str, int *str_len)
{
   if ((*str_len >= 4) && (strncmp(input_str, "sip:", 4) == 0)) {

      // input string beginning with "sip:"

      *output_str = input_str + 4 * sizeof (char);
      *str_len -= 4;

   } else {
      if ((*str_len >= 5) && (strncmp(input_str, "sips:", 5) == 0)) {

         // input string beginning with "sips:"

         *output_str = input_str + 5 * sizeof (char);
         *str_len -= 5;

      } else {
         return -1;
      }
   }

   // ignore ';' or '?' + string after it
   int i = 0;
   while (i < *str_len) {
      if (((*output_str)[i] == ';') || ((*output_str)[i] == '?')) {
         *str_len = i;
         break;
      }
      i++;
   }

   // set terminating null character
   (*output_str)[*str_len] = '\0';

   return 0;
}

void get_string_from_unirec(char *string_output, int *string_len, int unirec_field_id, int max_length, const void *in_rec, ur_template_t *in_tmplt)
{
   *string_len = ur_get_var_len(in_tmplt, in_rec, unirec_field_id);
   if (*string_len > max_length) 
      *string_len = max_length;

   memcpy(string_output, ur_get_ptr_by_id(in_tmplt, in_rec, unirec_field_id), *string_len);
   string_output[*string_len] = '\0';
}

int main(int argc, char **argv)
{
   int ret;
   char opt;
   uint16_t msg_type;
   char sip_from_orig[MAX_LENGTH_SIP_FROM + 1], sip_cseq[MAX_LENGTH_CSEQ + 1];
   int sip_cseq_len;
   void *tree_ipv4, *tree_ipv6;

   INIT_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS)
   TRAP_DEFAULT_INITIALIZATION(argc, argv, *module_info);
   TRAP_REGISTER_DEFAULT_SIGNAL_HANDLER();
   verbose = trap_get_verbose_level();

   ur_template_t *in_tmplt = ur_create_input_template(0, UNIREC_INPUT_TEMPLATE, NULL);
   if (in_tmplt == NULL) {
      fprintf(stderr, "Error: input template could not be created.\n");
      return -1;
   }

   trap_set_data_fmt(0, TRAP_FMT_JSON, "");

   while ((opt = TRAP_GETOPT(argc, argv, module_getopt_string, long_options)) != -1) {
      switch (opt) {
         case 'a': 
            sscanf(optarg,"%" SCNu64 "", &g_alert_threshold);
            if (g_alert_threshold < 1) {
               fprintf(stderr, "Error: irrational value of alert threshold.\n");
               goto cleanup;               
            }

            break;
         case 'c':
            sscanf(optarg,"%" SCNu64 "", &g_check_mem_interval);
            if (g_check_mem_interval < 1) {
               fprintf(stderr, "Error: irrational value of memory check intervals.\n");
               goto cleanup;               
            }

            break;
         case 'f':
            sscanf(optarg,"%" SCNu64 "", &g_free_mem_interval);
            if (g_check_mem_interval < 1) {
               fprintf(stderr, "Error: irrational value of memory deallocation after last attack action.\n");
               goto cleanup;               
            }

            break;
         default:
            fprintf(stderr, "Error: unsupported parameter.\n");
            goto cleanup;
      }
   }

   tree_ipv4 = b_plus_tree_initialize(5, &compare_ipv4, sizeof(attacked_server_t), IP_VERSION_4_BYTES);
   tree_ipv6 = b_plus_tree_initialize(5, &compare_ipv6, sizeof(attacked_server_t), IP_VERSION_6_BYTES);

   while (!stop) {
      const void *in_rec;
      uint16_t in_rec_size;
      sip_dataholder_t *sip_data;

      ret = TRAP_RECEIVE(0, in_rec, in_rec_size, in_tmplt);
      TRAP_DEFAULT_RECV_ERROR_HANDLING(ret, continue, break);
      if (in_rec_size < ur_rec_fixlen_size(in_tmplt)) {
         if (in_rec_size <= 1) {
            break; 
         } else {
            fprintf(stderr, "Error: data with wrong size received (expected size: >= %hu, received size: %hu)\n", ur_rec_fixlen_size(in_tmplt), in_rec_size);
            break;
         }
      }

      get_string_from_unirec(sip_cseq, &sip_cseq_len, F_SIP_CSEQ, MAX_LENGTH_CSEQ, in_rec, in_tmplt);
      if (!(sip_cseq_len > 2 && strstr(sip_cseq, "REG"))) {
         continue;
      }

      sip_data = (sip_dataholder_t*)malloc(sizeof(sip_dataholder_t));
      if (!sip_data) {
         fprintf(stderr, "Error: malloc failed.\n");
         break;
      }

      msg_type = ur_get(in_tmplt, in_rec, F_SIP_MSG_TYPE);
      sip_data->status_code = ur_get(in_tmplt, in_rec, F_SIP_STATUS_CODE);
      if (!(msg_type == SIP_MSG_TYPE_STATUS && (sip_data->status_code == SIP_STATUS_FORBIDDEN || sip_data->status_code == SIP_STATUS_OK))) {
         free(sip_data);
         continue;
      }

      get_string_from_unirec(sip_from_orig, &(sip_data->sip_from_len), F_SIP_CALLING_PARTY, MAX_LENGTH_SIP_FROM, in_rec, in_tmplt);
      int invalid_sipfrom = cut_sip_identifier(&(sip_data->sip_from), sip_from_orig, &(sip_data->sip_from_len));
      if (invalid_sipfrom) {
         free(sip_data);
         VERBOSE("Warning: invalid value of sip_from field.\n")
         continue;
      }

      sip_data->ip_src = &ur_get(in_tmplt, in_rec, F_SRC_IP);
      sip_data->ip_dst = &ur_get(in_tmplt, in_rec, F_DST_IP);
      if (ip_is_null(sip_data->ip_src) || ip_is_null(sip_data->ip_dst)) {
         free(sip_data);
         VERBOSE("Warning: null value of IP.\n")
         continue;
      }

      sip_data->time_stamp = ur_time_get_sec((ur_time_t*)ur_get(in_tmplt, in_rec, F_TIME_FIRST));
      sip_data->ipv4 = ip_is4(sip_data->ip_src);
      if (sip_data->ipv4) {
         sip_data->tree = tree_ipv4;
         sip_data->tree_key_length = IP_VERSION_4_BYTES;
         sip_data->comp_func = &compare_ipv4;
      } else {
         sip_data->tree = tree_ipv6;
         sip_data->tree_key_length = IP_VERSION_6_BYTES;
         sip_data->comp_func = &compare_ipv6;
      }

      int retval = insert_attack_attempt(sip_data);
      if (retval != 0) {
         free(sip_data);
         VERBOSE("Error: unable to insert possible attack attempt.\n")
         break;
      }

      if (!free_ceased_attacks(tree_ipv4, sip_data->time_stamp, IP_VERSION_4_BYTES) || !free_ceased_attacks(tree_ipv6, sip_data->time_stamp, IP_VERSION_6_BYTES)) {
         free(sip_data);
         VERBOSE("Error: free_ceased_attacks function failed.\n")
         break;
      }
      
      free(sip_data);
   }

   destroy_tree(tree_ipv4);
   destroy_tree(tree_ipv6);

cleanup:
   TRAP_DEFAULT_FINALIZATION();
   FREE_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS)
   ur_free_template(in_tmplt);
   ur_finalize();

   return 0;
}

