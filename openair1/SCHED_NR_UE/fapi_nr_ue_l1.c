/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this file
 * except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/* \file fapi_nr_ue_l1.c
 * \brief functions for NR UE FAPI-like interface
 * \author R. Knopp, K.H. HSU
 * \date 2018
 * \version 0.1
 * \company Eurecom / NTUST
 * \email: knopp@eurecom.fr, kai-hsiang.hsu@eurecom.fr
 * \note
 * \warning
 */

#include <stdio.h>

#include "fapi_nr_ue_interface.h"
#include "fapi_nr_ue_l1.h"
#include "harq_nr.h"
#include "openair2/NR_UE_PHY_INTERFACE/NR_IF_Module.h"
#include "PHY/defs_nr_UE.h"
#include "PHY/impl_defs_nr.h"
#include "utils.h"
#include "openair2/PHY_INTERFACE/queue_t.h"
#include "SCHED_NR_UE/phy_sch_processing_time.h"
#include "openair1/PHY/phy_extern_nr_ue.h"

const char *const dl_pdu_type[] = {"DCI", "DLSCH", "RA_DLSCH", "SI_DLSCH", "P_DLSCH", "CSI_RS", "CSI_IM", "TA"};
const char *const ul_pdu_type[] = {"PRACH", "PUCCH", "PUSCH", "SRS"};
queue_t nr_rx_ind_queue;
queue_t nr_crc_ind_queue;
queue_t nr_uci_ind_queue;
queue_t nr_rach_ind_queue;

static void fill_uci_2_3_4(nfapi_nr_uci_pucch_pdu_format_2_3_4_t *pdu_2_3_4,
                           fapi_nr_ul_config_pucch_pdu *pucch_pdu)
{
  NR_UE_MAC_INST_t *mac = get_mac_inst(0);
  memset(pdu_2_3_4, 0, sizeof(*pdu_2_3_4));
  pdu_2_3_4->handle = 0;
  pdu_2_3_4->rnti = pucch_pdu->rnti;
  pdu_2_3_4->pucch_format = 2;
  pdu_2_3_4->ul_cqi = 255;
  pdu_2_3_4->timing_advance = 0;
  pdu_2_3_4->rssi = 0;
  // TODO: Eventually check 38.212:Sect.631 to know when to use csi_part2, for now only using csi_part1
  pdu_2_3_4->pduBitmap = 4;
  pdu_2_3_4->csi_part1.csi_part1_bit_len = mac->nr_ue_emul_l1.num_csi_reports;
  int csi_part1_byte_len = (int)((pdu_2_3_4->csi_part1.csi_part1_bit_len / 8) + 1);
  AssertFatal(!pdu_2_3_4->csi_part1.csi_part1_payload, "pdu_2_3_4->csi_part1.csi_part1_payload != NULL\n");
  pdu_2_3_4->csi_part1.csi_part1_payload = CALLOC(csi_part1_byte_len,
                                                  sizeof(pdu_2_3_4->csi_part1.csi_part1_payload));
  for (int k = 0; k < csi_part1_byte_len; k++)
  {
    pdu_2_3_4->csi_part1.csi_part1_payload[k] = (pucch_pdu->payload >> (k * 8)) & 0xff;
  }
  pdu_2_3_4->csi_part1.csi_part1_crc = 0;
}

static void free_uci_inds(nfapi_nr_uci_indication_t *uci_ind)
{
    for (int k = 0; k < uci_ind->num_ucis; k++)
    {
        if (uci_ind->uci_list[k].pdu_type == NFAPI_NR_UCI_FORMAT_0_1_PDU_TYPE)
        {
            //nfapi_nr_uci_pucch_pdu_format_0_1_t *pdu_0_1 = &uci_ind->uci_list[k].pucch_pdu_format_0_1;
            // Warning: pdu_0_1 is unused
        }
        if (uci_ind->uci_list[k].pdu_type == NFAPI_NR_UCI_FORMAT_2_3_4_PDU_TYPE)
        {
            nfapi_nr_uci_pucch_pdu_format_2_3_4_t *pdu_2_3_4 = &uci_ind->uci_list[k].pucch_pdu_format_2_3_4;
            free(pdu_2_3_4->sr.sr_payload);
            pdu_2_3_4->sr.sr_payload = NULL;
            free(pdu_2_3_4->harq.harq_payload);
            pdu_2_3_4->harq.harq_payload = NULL;
        }
    }
    free(uci_ind->uci_list);
    uci_ind->uci_list = NULL;
    free(uci_ind);
}

int8_t nr_ue_scheduled_response_stub(nr_scheduled_response_t *scheduled_response) {

  NR_UE_MAC_INST_t *mac = get_mac_inst(0);

  if (scheduled_response && scheduled_response->ul_config) {
    int frame = scheduled_response->ul_config->frame;
    int slot = scheduled_response->ul_config->slot;
    fapi_nr_ul_config_request_pdu_t *it = fapiLockIterator(scheduled_response->ul_config, frame, slot);
    while (it->pdu_type != FAPI_NR_END) {
      switch (it->pdu_type) {
        case FAPI_NR_UL_CONFIG_TYPE_PRACH: {
          fapi_nr_ul_config_prach_pdu *prach_pdu = &it->prach_config_pdu;
          nfapi_nr_rach_indication_t *rach_ind = CALLOC(1, sizeof(*rach_ind));
          rach_ind->sfn = frame;
          rach_ind->slot = slot;
          rach_ind->header.message_id = NFAPI_NR_PHY_MSG_TYPE_RACH_INDICATION;
          uint8_t pdu_index = 0;
          rach_ind->pdu_list = CALLOC(1, sizeof(*rach_ind->pdu_list));
          rach_ind->number_of_pdus = 1;
          rach_ind->pdu_list[pdu_index].phy_cell_id = prach_pdu->phys_cell_id;
          rach_ind->pdu_list[pdu_index].symbol_index = prach_pdu->prach_start_symbol;
          rach_ind->pdu_list[pdu_index].slot_index = prach_pdu->prach_slot;
          rach_ind->pdu_list[pdu_index].freq_index = prach_pdu->num_ra;
          rach_ind->pdu_list[pdu_index].avg_rssi = 128;
          rach_ind->pdu_list[pdu_index].avg_snr = 0xff; // invalid for now
          const int num_p = rach_ind->pdu_list[pdu_index].num_preamble;
          AssertFatal(num_p == 1, "can handle only one preamble in preamble_list\n");
          rach_ind->pdu_list[pdu_index].num_preamble = 1;
          rach_ind->pdu_list[pdu_index].preamble_list[0].preamble_index = prach_pdu->ra_PreambleIndex;
          rach_ind->pdu_list[pdu_index].preamble_list[0].timing_advance = 0;
          rach_ind->pdu_list[pdu_index].preamble_list[0].preamble_pwr = 0xffffffff;

          if (!put_queue(&nr_rach_ind_queue, rach_ind)) {
            free(rach_ind->pdu_list);
            free(rach_ind);
          }
          LOG_D(NR_MAC, "We have successfully filled the rach_ind queue with the recently filled rach ind\n");
          break;
        }
        case (FAPI_NR_UL_CONFIG_TYPE_PUSCH): {
          nfapi_nr_rx_data_indication_t *rx_ind = CALLOC(1, sizeof(*rx_ind));
          nfapi_nr_crc_indication_t *crc_ind = CALLOC(1, sizeof(*crc_ind));
          nfapi_nr_ue_pusch_pdu_t *pusch_config_pdu = &it->pusch_config_pdu;
          if (pusch_config_pdu->tx_request_body.fapiTxPdu) {
            rx_ind->header.message_id = NFAPI_NR_PHY_MSG_TYPE_RX_DATA_INDICATION;
            rx_ind->sfn = frame;
            rx_ind->slot = slot;
            rx_ind->number_of_pdus = 1;
            rx_ind->pdu_list = CALLOC(rx_ind->number_of_pdus, sizeof(*rx_ind->pdu_list));
            for (int j = 0; j < rx_ind->number_of_pdus; j++) {
              fapi_nr_tx_request_body_t *tx_req_body = &pusch_config_pdu->tx_request_body;
              rx_ind->pdu_list[j].handle = pusch_config_pdu->handle;
              rx_ind->pdu_list[j].harq_id = pusch_config_pdu->pusch_data.harq_process_id;
              rx_ind->pdu_list[j].pdu_length = tx_req_body->pdu_length;
              rx_ind->pdu_list[j].pdu = CALLOC(tx_req_body->pdu_length, sizeof(*rx_ind->pdu_list[j].pdu));
              memcpy(rx_ind->pdu_list[j].pdu, tx_req_body->fapiTxPdu, tx_req_body->pdu_length * sizeof(*rx_ind->pdu_list[j].pdu));
              rx_ind->pdu_list[j].rnti = pusch_config_pdu->rnti;
              /* TODO: Implement channel modeling to abstract TA and CQI. For now,
                 we hard code the values below since they are set in L1 and we are
                 abstracting L1. */
              rx_ind->pdu_list[j].timing_advance = 31;
              rx_ind->pdu_list[j].ul_cqi = 255;
            }

            crc_ind->header.message_id = NFAPI_NR_PHY_MSG_TYPE_CRC_INDICATION;
            crc_ind->number_crcs = rx_ind->number_of_pdus;
            crc_ind->sfn = frame;
            crc_ind->slot = slot;
            crc_ind->crc_list = CALLOC(crc_ind->number_crcs, sizeof(*crc_ind->crc_list));
            for (int j = 0; j < crc_ind->number_crcs; j++) {
              crc_ind->crc_list[j].handle = pusch_config_pdu->handle;
              crc_ind->crc_list[j].harq_id = pusch_config_pdu->pusch_data.harq_process_id;
              LOG_D(NR_MAC, "This is the harq pid %d for crc_list[%d]\n", crc_ind->crc_list[j].harq_id, j);
              LOG_D(NR_MAC, "This is sched sfn/sl [%d %d] and crc sfn/sl [%d %d]\n", frame, slot, crc_ind->sfn, crc_ind->slot);
              crc_ind->crc_list[j].num_cb = pusch_config_pdu->pusch_data.num_cb;
              crc_ind->crc_list[j].rnti = pusch_config_pdu->rnti;
              crc_ind->crc_list[j].tb_crc_status = 0;
              crc_ind->crc_list[j].timing_advance = 31;
              crc_ind->crc_list[j].ul_cqi = 255;
              emul_l1_harq_t *harq = &mac->nr_ue_emul_l1.harq[crc_ind->crc_list[j].harq_id];
              AssertFatal(harq->active_ul_harq_sfn == -1 && harq->active_ul_harq_slot == -1,
                          "We did not send an active CRC when we should have!\n");
              harq->active_ul_harq_sfn = crc_ind->sfn;
              harq->active_ul_harq_slot = crc_ind->slot;
              LOG_D(NR_MAC,
                    "This is sched sfn/sl [%d %d] and crc sfn/sl [%d %d] with mcs_index in ul_cqi -> %d\n",
                    frame,
                    slot,
                    crc_ind->sfn,
                    crc_ind->slot,
                    pusch_config_pdu->mcs_index);
            }

            if (!put_queue(&nr_rx_ind_queue, rx_ind)) {
              LOG_E(NR_MAC, "Put_queue failed for rx_ind\n");
              for (int i = 0; i < rx_ind->number_of_pdus; i++) {
                free(rx_ind->pdu_list[i].pdu);
                rx_ind->pdu_list[i].pdu = NULL;
              }

              free(rx_ind->pdu_list);
              rx_ind->pdu_list = NULL;
              free(rx_ind);
              rx_ind = NULL;
            }
            if (!put_queue(&nr_crc_ind_queue, crc_ind)) {
              LOG_E(NR_MAC, "Put_queue failed for crc_ind\n");
              free(crc_ind->crc_list);
              crc_ind->crc_list = NULL;
              free(crc_ind);
              crc_ind = NULL;
            }

            LOG_D(PHY, "In %s: Filled queue rx/crc_ind which was filled by ulconfig. \n", __FUNCTION__);
          }
          break;
        }
        case FAPI_NR_UL_CONFIG_TYPE_PUCCH: {
          nfapi_nr_uci_indication_t *uci_ind = CALLOC(1, sizeof(*uci_ind));
          uci_ind->header.message_id = NFAPI_NR_PHY_MSG_TYPE_UCI_INDICATION;
          uci_ind->sfn = frame;
          uci_ind->slot = slot;
          uci_ind->num_ucis = 1;
          uci_ind->uci_list = CALLOC(uci_ind->num_ucis, sizeof(*uci_ind->uci_list));
          for (int j = 0; j < uci_ind->num_ucis; j++) {
            LOG_D(NR_MAC, "pucch_config_pdu.n_bit = %d\n", it->pucch_config_pdu.n_bit);
            if (it->pucch_config_pdu.n_bit > 3 && mac->nr_ue_emul_l1.num_csi_reports > 0) {
              uci_ind->uci_list[j].pdu_type = NFAPI_NR_UCI_FORMAT_2_3_4_PDU_TYPE;
              uci_ind->uci_list[j].pdu_size = sizeof(nfapi_nr_uci_pucch_pdu_format_2_3_4_t);
              nfapi_nr_uci_pucch_pdu_format_2_3_4_t *pdu_2_3_4 = &uci_ind->uci_list[j].pucch_pdu_format_2_3_4;
              fill_uci_2_3_4(pdu_2_3_4, &it->pucch_config_pdu);
            } else {
              nfapi_nr_uci_pucch_pdu_format_0_1_t *pdu_0_1 = &uci_ind->uci_list[j].pucch_pdu_format_0_1;
              uci_ind->uci_list[j].pdu_type = NFAPI_NR_UCI_FORMAT_0_1_PDU_TYPE;
              uci_ind->uci_list[j].pdu_size = sizeof(nfapi_nr_uci_pucch_pdu_format_0_1_t);
              memset(pdu_0_1, 0, sizeof(*pdu_0_1));
              pdu_0_1->handle = 0;
              pdu_0_1->rnti = it->pucch_config_pdu.rnti;
              pdu_0_1->pucch_format = 1;
              pdu_0_1->ul_cqi = 255;
              pdu_0_1->timing_advance = 0;
              pdu_0_1->rssi = 0;
              if (mac->nr_ue_emul_l1.num_harqs > 0) {
                int harq_index = 0;
                pdu_0_1->pduBitmap = 2; // (value->pduBitmap >> 1) & 0x01) == HARQ and (value->pduBitmap) & 0x01) == SR
                pdu_0_1->harq.num_harq = mac->nr_ue_emul_l1.num_harqs;
                pdu_0_1->harq.harq_confidence_level = 0;
                int harq_pid = -1;
                for (int k = 0; k < NR_MAX_HARQ_PROCESSES; k++) {
                  if (mac->nr_ue_emul_l1.harq[k].active && mac->nr_ue_emul_l1.harq[k].active_dl_harq_sfn == uci_ind->sfn
                      && mac->nr_ue_emul_l1.harq[k].active_dl_harq_slot == uci_ind->slot) {
                    mac->nr_ue_emul_l1.harq[k].active = false;
                    harq_pid = k;
                    AssertFatal(harq_index < pdu_0_1->harq.num_harq, "Invalid harq_index %d\n", harq_index);
                    pdu_0_1->harq.harq_list[harq_index].harq_value = !mac->dl_harq_info[k].ack;
                    harq_index++;
                  }
                }
                AssertFatal(harq_pid != -1, "No active harq_pid, sfn_slot = %u.%u", uci_ind->sfn, uci_ind->slot);
              }
            }
          }

          LOG_D(NR_PHY, "Sending UCI with %d PDUs in sfn.slot %d/%d\n", uci_ind->num_ucis, uci_ind->sfn, uci_ind->slot);
          NR_UL_IND_t ul_info = {
              .uci_ind = *uci_ind,
          };
          send_nsa_standalone_msg(&ul_info, uci_ind->header.message_id);
          free_uci_inds(uci_ind);
          break;
        }

        default:
          LOG_W(NR_MAC, "Unknown ul_config->pdu_type %d\n", it->pdu_type);
          break;
      }
      it++;
    }
    release_ul_config(it, true);
  }
  return 0;
}

static void configure_dlsch(NR_UE_DLSCH_t *dlsch0,
                            NR_DL_UE_HARQ_t *harq_list,
                            fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config_pdu,
                            NR_UE_MAC_INST_t *mac,
                            int rnti)
{
  // Temporary code to process type0 as type1 when the RB allocation is contiguous
  if (dlsch_config_pdu->resource_alloc == 0) {
    dlsch_config_pdu->number_rbs = count_bits(dlsch_config_pdu->rb_bitmap, sizeofArray(dlsch_config_pdu->rb_bitmap));
    int state = 0;
    for (int i = 0; i < sizeof(dlsch_config_pdu->rb_bitmap) * 8; i++) {
      int allocated = dlsch_config_pdu->rb_bitmap[i / 8] & (1 << (i % 8));
      if (allocated) {
        if (state == 0) {
          dlsch_config_pdu->start_rb = i;
          state = 1;
        } else
          AssertFatal(state == 1, "non-contiguous RB allocation in RB allocation type 0 not implemented");
      } else {
        if (state == 1) {
          state = 2;
        }
      }
    }
  }

  const uint8_t current_harq_pid = dlsch_config_pdu->harq_process_nbr;
  dlsch0->active = true;
  dlsch0->rnti = rnti;

  LOG_D(PHY,"current_harq_pid = %d\n", current_harq_pid);

  NR_DL_UE_HARQ_t *dlsch0_harq = &harq_list[current_harq_pid];

  //get nrOfLayers from DCI info
  uint8_t Nl = 0;
  for (int i = 0; i < 12; i++) { // max 12 ports
    if ((dlsch_config_pdu->dmrs_ports >> i) & 0x01)
      Nl += 1;
  }
  if (Nl == 0) {
    LOG_E(PHY, "Invalid number of layers %d for DLSCH\n", Nl);
    // setting NACK for this TB
    dlsch0->active = false;
    update_harq_status(mac, current_harq_pid, 0);
    return;
  }
  dlsch0->Nl = Nl;
  if (dlsch_config_pdu->new_data_indicator) {
    dlsch0_harq->first_rx = true;
    dlsch0_harq->DLround = 0;
  } else {
    dlsch0_harq->first_rx = false;
    dlsch0_harq->DLround++;
  }
  downlink_harq_process(dlsch0_harq, current_harq_pid, dlsch_config_pdu->new_data_indicator, dlsch_config_pdu->rv, dlsch0->rnti_type);
  if (dlsch0_harq->status != ACTIVE) {
    // dlsch0_harq->status not ACTIVE due to false retransmission
    // Reset the following flag to skip PDSCH procedures in that case and retrasmit harq status
    dlsch0->active = false;
    LOG_W(NR_MAC, "dlsch0_harq->status not ACTIVE due to false retransmission harq pid: %d\n", current_harq_pid);
    update_harq_status(mac, current_harq_pid, dlsch0_harq->decodeResult);
  }
}

static void configure_ntn_params(PHY_VARS_NR_UE *ue, fapi_nr_dl_ntn_config_command_pdu* ntn_params_message)
{
  if (!ue->ntn_config_message) {
    ue->ntn_config_message = CALLOC(1, sizeof(*ue->ntn_config_message));
  }

  ue->ntn_config_message->ntn_config_params.cell_specific_k_offset = ntn_params_message->cell_specific_k_offset;
  ue->ntn_config_message->ntn_config_params.N_common_ta_adj = ntn_params_message->N_common_ta_adj;
  ue->ntn_config_message->ntn_config_params.ntn_ta_commondrift = ntn_params_message->ntn_ta_commondrift;
  ue->ntn_config_message->ntn_config_params.ntn_total_time_advance_ms = ntn_params_message->ntn_total_time_advance_ms;
  ue->ntn_config_message->update = true;
}

static void configure_ta_command(PHY_VARS_NR_UE *ue, fapi_nr_ta_command_pdu *ta_command_pdu)
{
  /* Time Alignment procedure
  // - UE processing capability 1
  // - Setting the TA update to be applied after the reception of the TA command
  // - Timing adjustment computed according to TS 38.213 section 4.2
  // - Durations of N1 and N2 symbols corresponding to PDSCH and PUSCH are
  //   computed according to sections 5.3 and 6.4 of TS 38.214 */
  const int numerology = ue->frame_parms.numerology_index;
  const int ofdm_symbol_size = ue->frame_parms.ofdm_symbol_size;
  const int nb_prefix_samples = ue->frame_parms.nb_prefix_samples;
  const int samples_per_subframe = ue->frame_parms.samples_per_subframe;
  const int slots_per_frame = ue->frame_parms.slots_per_frame;
  const int slots_per_subframe = ue->frame_parms.slots_per_subframe;

  const double tc_factor = 1.0 / samples_per_subframe;
  // convert time factor "16 * 64 * T_c / (2^mu)" in N_TA calculation in TS38.213 section 4.2 to samples by multiplying with samples per second
  //   16 * 64 * T_c            / (2^mu) * samples_per_second
  // = 16 * T_s                 / (2^mu) * samples_per_second
  // = 16 * 1 / (15 kHz * 2048) / (2^mu) * (15 kHz * 2^mu * ofdm_symbol_size)
  // = 16 * 1 /           2048           *                  ofdm_symbol_size
  // = 16 * ofdm_symbol_size / 2048
  uint16_t bw_scaling = 16 * ofdm_symbol_size / 2048;

  const int Ta_max = 3846; // Max value of 12 bits TA Command
  const double N_TA_max = Ta_max * bw_scaling * tc_factor;

  // symbols corresponding to a PDSCH processing time for UE processing capability 1
  // when additional PDSCH DM-RS is configured
  int N_1 = pdsch_N_1_capability_1[numerology][3];

  /* PUSCH preapration time N_2 for processing capability 1 */
  const int N_2 = pusch_N_2_timing_capability_1[numerology][1];

  /* N_t_1 time duration in msec of N_1 symbols corresponding to a PDSCH reception time
  // N_t_2 time duration in msec of N_2 symbols corresponding to a PUSCH preparation time */
  double N_t_1 = N_1 * (ofdm_symbol_size + nb_prefix_samples) * tc_factor;
  double N_t_2 = N_2 * (ofdm_symbol_size + nb_prefix_samples) * tc_factor;

  /* Time alignment procedure */
  // N_t_1 + N_t_2 + N_TA_max must be in msec
  const double t_subframe = 1.0; // subframe duration of 1 msec
  const int ul_tx_timing_adjustment = 1 + (int)ceil(slots_per_subframe * (N_t_1 + N_t_2 + N_TA_max + 0.5) / t_subframe);

  if (ta_command_pdu->is_rar) {
    ue->ta_slot = ta_command_pdu->ta_slot;
    ue->ta_frame = ta_command_pdu->ta_frame;
    ue->ta_command = ta_command_pdu->ta_command + 31; // To use TA adjustment algo in ue_ta_procedures()
  } else {
    ue->ta_slot = (ta_command_pdu->ta_slot + ul_tx_timing_adjustment) % slots_per_frame;
    if (ta_command_pdu->ta_slot + ul_tx_timing_adjustment > slots_per_frame)
      ue->ta_frame = (ta_command_pdu->ta_frame + 1) % 1024;
    else
      ue->ta_frame = ta_command_pdu->ta_frame;
    ue->ta_command = ta_command_pdu->ta_command;
  }

  LOG_D(PHY,
        "TA command received in %d.%d Starting UL time alignment procedures. TA update will be applied at frame %d slot %d\n",
        ta_command_pdu->ta_frame, ta_command_pdu->ta_slot, ue->ta_frame, ue->ta_slot);
}

static void nr_ue_scheduled_response_dl(NR_UE_MAC_INST_t *mac,
                                        PHY_VARS_NR_UE *phy,
                                        fapi_nr_dl_config_request_t *dl_config,
                                        nr_phy_data_t *phy_data)
{
  AssertFatal(dl_config->number_pdus < FAPI_NR_DL_CONFIG_LIST_NUM,
              "dl_config->number_pdus %d out of bounds\n",
              dl_config->number_pdus);

  for (int i = 0; i < dl_config->number_pdus; ++i) {
    fapi_nr_dl_config_request_pdu_t *pdu = dl_config->dl_config_list + i;
    AssertFatal(pdu->pdu_type <= FAPI_NR_DL_CONFIG_TYPES, "pdu_type %d\n", pdu->pdu_type);
    LOG_D(PHY, "Copying DL %s PDU of %d total DL PDUs:\n", dl_pdu_type[pdu->pdu_type - 1], dl_config->number_pdus);

    switch (pdu->pdu_type) {
      case FAPI_NR_DL_CONFIG_TYPE_DCI:
        AssertFatal(phy_data->phy_pdcch_config.nb_search_space < FAPI_NR_MAX_SS, "Fix array size not large enough\n");
        const int nextFree = phy_data->phy_pdcch_config.nb_search_space;
        phy_data->phy_pdcch_config.pdcch_config[nextFree] = pdu->dci_config_pdu.dci_config_rel15;
        phy_data->phy_pdcch_config.nb_search_space++;
        LOG_D(PHY, "Number of DCI SearchSpaces %d\n", phy_data->phy_pdcch_config.nb_search_space);
        break;
      case FAPI_NR_DL_CONFIG_TYPE_CSI_IM:
        phy_data->csiim_vars.csiim_config_pdu = pdu->csiim_config_pdu.csiim_config_rel15;
        phy_data->csiim_vars.active = true;
        break;
      case FAPI_NR_DL_CONFIG_TYPE_CSI_RS:
        phy_data->csirs_vars.csirs_config_pdu = pdu->csirs_config_pdu.csirs_config_rel15;
        phy_data->csirs_vars.active = true;
        break;
      case FAPI_NR_DL_CONFIG_TYPE_RA_DLSCH: {
        fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config_pdu = &pdu->dlsch_config_pdu.dlsch_config_rel15;
        NR_UE_DLSCH_t *dlsch0 = phy_data->dlsch + 0;
        dlsch0->rnti_type = TYPE_RA_RNTI_;
        dlsch0->dlsch_config = *dlsch_config_pdu;
        configure_dlsch(dlsch0, phy->dl_harq_processes[0], dlsch_config_pdu, mac, pdu->dlsch_config_pdu.rnti);
      } break;
      case FAPI_NR_DL_CONFIG_TYPE_SI_DLSCH: {
        fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config_pdu = &pdu->dlsch_config_pdu.dlsch_config_rel15;
        NR_UE_DLSCH_t *dlsch0 = phy_data->dlsch + 0;
        dlsch0->rnti_type = TYPE_SI_RNTI_;
        dlsch0->dlsch_config = *dlsch_config_pdu;
        configure_dlsch(dlsch0, phy->dl_harq_processes[0], dlsch_config_pdu, mac, pdu->dlsch_config_pdu.rnti);
      } break;
      case FAPI_NR_DL_CONFIG_TYPE_DLSCH: {
        fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config_pdu = &pdu->dlsch_config_pdu.dlsch_config_rel15;
        NR_UE_DLSCH_t *dlsch0 = &phy_data->dlsch[0];
        dlsch0->rnti_type = TYPE_C_RNTI_;
        dlsch0->dlsch_config = *dlsch_config_pdu;
        configure_dlsch(dlsch0, phy->dl_harq_processes[0], dlsch_config_pdu, mac, pdu->dlsch_config_pdu.rnti);
      } break;
      case FAPI_NR_CONFIG_TA_COMMAND:
        configure_ta_command(phy, &pdu->ta_command_pdu);
        break;
      case FAPI_NR_DL_NTN_CONFIG_PARAMS:
        configure_ntn_params(phy, &pdu->ntn_config_command_pdu);
        break;
      default:
        LOG_W(PHY, "unhandled dl pdu type %d \n", pdu->pdu_type);
    }
  }
  dl_config->number_pdus = 0;
}

static void nr_ue_scheduled_response_ul(PHY_VARS_NR_UE *phy, fapi_nr_ul_config_request_t *ul_config, nr_phy_data_tx_t *phy_data)
{
  fapi_nr_ul_config_request_pdu_t *pdu = fapiLockIterator(ul_config, ul_config->frame, ul_config->slot);
  if (!pdu) {
    LOG_E(NR_MAC, "Error in locking ul scheduler dtata\n");
    return;
  }

  while (pdu->pdu_type != FAPI_NR_END) {
    switch (pdu->pdu_type) {
      case FAPI_NR_UL_CONFIG_TYPE_PUSCH: {
        // pusch config pdu
        int current_harq_pid = pdu->pusch_config_pdu.pusch_data.harq_process_id;
        NR_UL_UE_HARQ_t *harq_process_ul_ue = &phy->ul_harq_processes[current_harq_pid];
        nfapi_nr_ue_pusch_pdu_t *pusch_pdu = &phy_data->ulsch.pusch_pdu;
        LOG_D(PHY,
              "copy pusch_config_pdu nrOfLayers: %d, num_dmrs_cdm_grps_no_data: %d\n",
              pdu->pusch_config_pdu.nrOfLayers,
              pdu->pusch_config_pdu.num_dmrs_cdm_grps_no_data);

        memcpy(pusch_pdu, &pdu->pusch_config_pdu, sizeof(*pusch_pdu));
        if (pdu->pusch_config_pdu.tx_request_body.fapiTxPdu) {
          LOG_D(PHY,
                "%d.%d Copying %d bytes to harq_process_ul_ue->a (harq_pid %d)\n",
                ul_config->frame,
                ul_config->slot,
                pdu->pusch_config_pdu.tx_request_body.pdu_length,
                current_harq_pid);
          memcpy(harq_process_ul_ue->payload_AB,
                 pdu->pusch_config_pdu.tx_request_body.fapiTxPdu,
                 pdu->pusch_config_pdu.tx_request_body.pdu_length);
        }

        phy_data->ulsch.status = ACTIVE;
        pdu->pdu_type = FAPI_NR_UL_CONFIG_TYPE_DONE; // not handle it any more
      } break;

      case FAPI_NR_UL_CONFIG_TYPE_PUCCH: {
        bool found = false;
        for (int j = 0; j < 2; j++) {
          if (phy_data->pucch_vars.active[j] == false) {
            LOG_D(PHY, "Copying pucch pdu to UE PHY\n");
            phy_data->pucch_vars.pucch_pdu[j] = pdu->pucch_config_pdu;
            phy_data->pucch_vars.active[j] = true;
            found = true;
            pdu->pdu_type = FAPI_NR_UL_CONFIG_TYPE_DONE; // not handle it any more
            break;
          }
        }
        if (!found)
          LOG_E(PHY, "Couldn't find allocation for PUCCH PDU in PUCCH VARS\n");
      } break;

      case FAPI_NR_UL_CONFIG_TYPE_PRACH: {
        phy->prach_vars[0]->prach_pdu = pdu->prach_config_pdu;
        phy->prach_vars[0]->active = true;
        pdu->pdu_type = FAPI_NR_UL_CONFIG_TYPE_DONE; // not handle it any more
      } break;

      case FAPI_NR_UL_CONFIG_TYPE_DONE:
        break;

      case FAPI_NR_UL_CONFIG_TYPE_SRS:
        // srs config pdu
        phy_data->srs_vars.srs_config_pdu = pdu->srs_config_pdu;
        phy_data->srs_vars.active = true;
        pdu->pdu_type = FAPI_NR_UL_CONFIG_TYPE_DONE; // not handle it any more
        break;

      default:
        LOG_W(PHY, "unhandled ul pdu type %d \n", pdu->pdu_type);
        break;
    }
    pdu++;
  }

  LOG_D(PHY, "clear ul_config\n");
  release_ul_config(pdu, true);
}

int8_t nr_ue_scheduled_response(nr_scheduled_response_t *scheduled_response)
{
  PHY_VARS_NR_UE *phy = PHY_vars_UE_g[scheduled_response->module_id][scheduled_response->CC_id];
  AssertFatal(!scheduled_response->dl_config || !scheduled_response->ul_config || !scheduled_response->sl_rx_config
                  || !scheduled_response->sl_tx_config,
              "phy_data parameter will be cast to two different types!\n");

  if (scheduled_response->dl_config)
    nr_ue_scheduled_response_dl(scheduled_response->mac,
                                phy,
                                scheduled_response->dl_config,
                                (nr_phy_data_t *)scheduled_response->phy_data);
  if (scheduled_response->ul_config)
    nr_ue_scheduled_response_ul(phy, scheduled_response->ul_config, (nr_phy_data_tx_t *)scheduled_response->phy_data);

  if (scheduled_response->sl_rx_config || scheduled_response->sl_tx_config) {
    sl_handle_scheduled_response(scheduled_response);
  }

  return 0;
}

void nr_ue_phy_config_request(nr_phy_config_t *phy_config)
{
  PHY_VARS_NR_UE *phy = PHY_vars_UE_g[phy_config->Mod_id][phy_config->CC_id];
  fapi_nr_config_request_t *nrUE_config = &phy->nrUE_config;
  if(phy_config != NULL) {
    phy->received_config_request = true;
    memcpy(nrUE_config, &phy_config->config_req, sizeof(fapi_nr_config_request_t));
  }
}

void nr_ue_synch_request(nr_synch_request_t *synch_request)
{
  fapi_nr_synch_request_t *synch_req = &PHY_vars_UE_g[synch_request->Mod_id][synch_request->CC_id]->synch_request.synch_req;
  memcpy(synch_req, &synch_request->synch_req, sizeof(fapi_nr_synch_request_t));
  PHY_vars_UE_g[synch_request->Mod_id][synch_request->CC_id]->synch_request.received_synch_request = 1;
}

void nr_ue_sl_phy_config_request(nr_sl_phy_config_t *phy_config)
{
  PHY_VARS_NR_UE *phy = PHY_vars_UE_g[phy_config->Mod_id][phy_config->CC_id];
  sl_nr_phy_config_request_t *sl_config = &phy->SL_UE_PHY_PARAMS.sl_config;
  if (phy_config != NULL) {
    phy->received_config_request = true;
    memcpy(sl_config, &phy_config->sl_config_req, sizeof(sl_nr_phy_config_request_t));
  }
}

/*
 * MAC sends the scheduled response with either TX configrequest for Sidelink Transmission requests
 * or RX config request for Sidelink Reception requests.
 * This procedure handles these TX/RX config requests received in this slot and configures PHY
 * with a TTI action to be performed in this slot(TTI)
 */
void sl_handle_scheduled_response(nr_scheduled_response_t *scheduled_response)
{
  module_id_t module_id = scheduled_response->module_id;
  const char *sl_rx_action[] = {"NONE", "RX_PSBCH", "RX_PSCCH", "RX_SCI2_ON_PSSCH", "RX_SLSCH_ON_PSSCH"};
  const char *sl_tx_action[] = {"TX_PSBCH", "TX_PSCCH_PSSCH", "TX_PSFCH"};

  if (scheduled_response->sl_rx_config != NULL) {
    sl_nr_rx_config_request_t *sl_rx_config = scheduled_response->sl_rx_config;
    nr_phy_data_t *phy_data = (nr_phy_data_t *)scheduled_response->phy_data;

    AssertFatal(sl_rx_config->number_pdus == SL_NR_RX_CONFIG_LIST_NUM, "sl_rx_config->number_pdus incorrect\n");

    switch (sl_rx_config->sl_rx_config_list[0].pdu_type) {
      case SL_NR_CONFIG_TYPE_RX_PSBCH:
        phy_data->sl_rx_action = SL_NR_CONFIG_TYPE_RX_PSBCH;
        LOG_D(PHY, "Recvd CONFIG_TYPE_RX_PSBCH\n");
        break;
      default:
        AssertFatal(0, "Incorrect sl_rx config req pdutype \n");
        break;
    }

    LOG_D(PHY,
          "[UE%d] TTI %d:%d, SL-RX action:%s\n",
          module_id,
          sl_rx_config->sfn,
          sl_rx_config->slot,
          sl_rx_action[phy_data->sl_rx_action]);

  } else if (scheduled_response->sl_tx_config != NULL) {
    sl_nr_tx_config_request_t *sl_tx_config = scheduled_response->sl_tx_config;
    nr_phy_data_tx_t *phy_data_tx = (nr_phy_data_tx_t *)scheduled_response->phy_data;

    AssertFatal(sl_tx_config->number_pdus == SL_NR_TX_CONFIG_LIST_NUM, "sl_tx_config->number_pdus incorrect \n");

    switch (sl_tx_config->tx_config_list[0].pdu_type) {
      case SL_NR_CONFIG_TYPE_TX_PSBCH:
        phy_data_tx->sl_tx_action = SL_NR_CONFIG_TYPE_TX_PSBCH;
        LOG_D(PHY, "Recvd CONFIG_TYPE_TX_PSBCH\n");
        *((uint32_t *)phy_data_tx->psbch_vars.psbch_payload) =
            *((uint32_t *)sl_tx_config->tx_config_list[0].tx_psbch_config_pdu.psbch_payload);
        phy_data_tx->psbch_vars.psbch_tx_power = sl_tx_config->tx_config_list[0].tx_psbch_config_pdu.psbch_tx_power;
        phy_data_tx->psbch_vars.tx_slss_id = sl_tx_config->tx_config_list[0].tx_psbch_config_pdu.tx_slss_id;
        break;
      default:
        AssertFatal(0, "Incorrect sl_tx config req pdutype \n");
        break;
    }

    LOG_D(PHY,
          "[UE%d] TTI %d:%d, SL-TX action:%s slss_id:%d, sl-mib:%x, psbch pwr:%d\n",
          module_id,
          sl_tx_config->sfn,
          sl_tx_config->slot,
          sl_tx_action[phy_data_tx->sl_tx_action - 6],
          phy_data_tx->psbch_vars.tx_slss_id,
          *((uint32_t *)phy_data_tx->psbch_vars.psbch_payload),
          phy_data_tx->psbch_vars.psbch_tx_power);
  }

}
