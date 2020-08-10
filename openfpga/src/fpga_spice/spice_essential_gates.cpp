/************************************************
 * This file includes functions on 
 * outputting Verilog netlists for essential gates
 * which are inverters, buffers, transmission-gates
 * logic gates etc. 
 ***********************************************/
#include <fstream>
#include <cmath>
#include <iomanip>

/* Headers from vtrutil library */
#include "vtr_assert.h"
#include "vtr_log.h"

/* Headers from openfpgashell library */
#include "command_exit_codes.h"

/* Headers from openfpgautil library */
#include "openfpga_digest.h"

#include "circuit_library_utils.h"

#include "spice_constants.h"
#include "spice_writer_utils.h"
#include "spice_essential_gates.h"

/* begin namespace openfpga */
namespace openfpga {

/********************************************************************
 * Print a SPICE model wrapper for a transistor model
 *******************************************************************/
static 
int print_spice_transistor_model_wrapper(std::fstream& fp,
                                         const TechnologyLibrary& tech_lib,
                                         const TechnologyModelId& model) {

  if (false == valid_file_stream(fp)) {
    return CMD_EXEC_FATAL_ERROR;
  }

  /* Transistor model followed a fixed port mapping
   * [X|M]<MODEL_CARD_NAME> <DRAIN> <GATE> <SOURCE> <BULK> 
   * which is a standard in SPICE modeling
   * We will output the pmos and nmos transistors wrappers
   * which are defined in this model
   */
  for (int itype = TECH_LIB_TRANSISTOR_PMOS;
       itype < NUM_TECH_LIB_TRANSISTOR_TYPES;
       ++itype) {
    const e_tech_lib_transistor_type& trans_type = static_cast<e_tech_lib_transistor_type>(itype); 
    fp << ".subckt ";
    fp << tech_lib.transistor_model_name(model, trans_type) << TRANSISTOR_WRAPPER_POSTFIX; 
    fp << " drain gate source bulk";
    fp << " L=" << std::setprecision(10) << tech_lib.transistor_model_chan_length(model, trans_type); 
    fp << " W=" << std::setprecision(10) << tech_lib.transistor_model_min_width(model, trans_type); 
    fp << "\n";

    fp << tech_lib.model_ref(model);
    fp << "1";
    fp << " drain gate source bulk";
    fp << " " << tech_lib.transistor_model_name(model, trans_type);
    fp << " L=L W=W";
    fp << "\n";

    fp << ".ends";
    fp << "\n";
  }

  return CMD_EXEC_SUCCESS;
}

/********************************************************************
 * Generate the SPICE netlist for transistors
 *******************************************************************/
int print_spice_transistor_wrapper(NetlistManager& netlist_manager,
                                   const TechnologyLibrary& tech_lib,
                                   const std::string& submodule_dir) {
  std::string spice_fname = submodule_dir + std::string(TRANSISTORS_SPICE_FILE_NAME);

  std::fstream fp;

  /* Create the file stream */
  fp.open(spice_fname, std::fstream::out | std::fstream::trunc);
  /* Check if the file stream if valid or not */
  check_file_stream(spice_fname.c_str(), fp); 

  /* Create file */
  VTR_LOG("Generating SPICE netlist '%s' for transistors...",
          spice_fname.c_str()); 

  print_spice_file_header(fp, std::string("Transistor wrappers"));

  /* Iterate over the transistor models */
  for (const TechnologyModelId& model : tech_lib.models()) {
    /* Focus on transistor model */
    if (TECH_LIB_MODEL_TRANSISTOR != tech_lib.model_type(model)) {
      continue;
    }
    /* Write a wrapper for the transistor model */
    if (CMD_EXEC_SUCCESS == print_spice_transistor_model_wrapper(fp, tech_lib, model)) {
      return CMD_EXEC_FATAL_ERROR;
    }
  } 

  /* Close file handler*/
  fp.close();

  /* Add fname to the netlist name list */
  NetlistId nlist_id = netlist_manager.add_netlist(spice_fname);
  VTR_ASSERT(NetlistId::INVALID() != nlist_id);
  netlist_manager.set_netlist_type(nlist_id, NetlistManager::SUBMODULE_NETLIST);

  VTR_LOG("Done\n");

  return CMD_EXEC_SUCCESS;
}

/********************************************************************
 * Generate the SPICE modeling for a power-gated inverter
 *
 * This function is created to be shared by inverter and buffer SPICE netlist writer
 *
 * Note: 
 * - This function does NOT create a file
 *   but requires a file stream created
 * - This function only output SPICE modeling for 
 *   an inverter. Any preprocessing or subckt definition should not be included!
 *******************************************************************/
static 
int print_spice_powergated_inverter_pmos_modeling(std::fstream& fp,
                                                  const std::string& trans_name_postfix,
                                                  const std::string& input_port_name,
                                                  const std::string& output_port_name,
                                                  const CircuitLibrary& circuit_lib,
                                                  const CircuitPortId& enb_port,
                                                  const TechnologyLibrary& tech_lib,
                                                  const TechnologyModelId& tech_model,
                                                  const float& trans_width) {

  if (false == valid_file_stream(fp)) {
    return CMD_EXEC_FATAL_ERROR;
  }

  /* Write power-gating transistor pairs using the technology model
   * Note that for a mulit-bit power gating port, we should cascade the transistors
   */
  bool first_enb_pin = true;
  size_t last_enb_pin;
  for (const auto& power_gate_pin : circuit_lib.pins(enb_port)) {
    BasicPort enb_pin(circuit_lib.port_prefix(enb_port), power_gate_pin, power_gate_pin);
    fp << "Xpmos_powergate_" << trans_name_postfix << "_pin_" << power_gate_pin << " ";
    /* For the first pin, we should connect it to local VDD*/
    if (true == first_enb_pin) {
      fp << output_port_name << "_pmos_pg_" << power_gate_pin << " "; 
      fp << generate_spice_port(enb_pin) << " "; 
      fp << "LVDD "; 
      fp << "LVDD "; 
      first_enb_pin = false;
    } else {
      VTR_ASSERT_SAFE(false == first_enb_pin);
      fp << output_port_name << "_pmos_pg_" << last_enb_pin << " "; 
      fp << generate_spice_port(enb_pin) << " "; 
      fp << output_port_name << "_pmos_pg_" << power_gate_pin << " "; 
      fp << "LVDD "; 
    }
    fp << tech_lib.transistor_model_name(tech_model, TECH_LIB_TRANSISTOR_PMOS) << TRANSISTOR_WRAPPER_POSTFIX; 
    fp << " W=" << std::setprecision(10) << trans_width;
    fp << "\n";

    /* Cache the last pin*/
    last_enb_pin = power_gate_pin;
  }

  /* Write transistor pairs using the technology model */
  fp << "Xpmos_" << trans_name_postfix << " ";
  fp << output_port_name << " "; 
  fp << input_port_name << " "; 
  fp << output_port_name << "_pmos_pg_" << circuit_lib.pins(enb_port).back() << " "; 
  fp << "LVDD "; 
  fp << tech_lib.transistor_model_name(tech_model, TECH_LIB_TRANSISTOR_PMOS) << TRANSISTOR_WRAPPER_POSTFIX; 
  fp << " W=" << std::setprecision(10) << trans_width;
  fp << "\n";

  return CMD_EXEC_SUCCESS;
}

/********************************************************************
 * Generate the SPICE modeling for the NMOS part of a power-gated inverter
 *
 * This function is created to be shared by inverter and buffer SPICE netlist writer
 *
 * Note: 
 * - This function does NOT create a file
 *   but requires a file stream created
 * - This function only output SPICE modeling for 
 *   an inverter. Any preprocessing or subckt definition should not be included!
 *******************************************************************/
static 
int print_spice_powergated_inverter_nmos_modeling(std::fstream& fp,
                                                  const std::string& trans_name_postfix,
                                                  const std::string& input_port_name,
                                                  const std::string& output_port_name,
                                                  const CircuitLibrary& circuit_lib,
                                                  const CircuitPortId& en_port,
                                                  const TechnologyLibrary& tech_lib,
                                                  const TechnologyModelId& tech_model,
                                                  const float& trans_width) {

  if (false == valid_file_stream(fp)) {
    return CMD_EXEC_FATAL_ERROR;
  }

  bool first_en_pin = true;
  size_t last_en_pin;
  for (const auto& power_gate_pin : circuit_lib.pins(en_port)) {
    BasicPort en_pin(circuit_lib.port_prefix(en_port), power_gate_pin, power_gate_pin);
    fp << "Xnmos_powergate_" << trans_name_postfix << "_pin_" << power_gate_pin << " ";
    /* For the first pin, we should connect it to local VDD*/
    if (true == first_en_pin) {
      fp << output_port_name << "_nmos_pg_" << power_gate_pin << " "; 
      fp << generate_spice_port(en_pin) << " "; 
      fp << "LGND "; 
      fp << "LGND "; 
      first_en_pin = false;
    } else {
      VTR_ASSERT_SAFE(false == first_en_pin);
      fp << output_port_name << "_nmos_pg_" << last_en_pin << " "; 
      fp << circuit_lib.port_prefix(en_port) << " "; 
      fp << output_port_name << "_nmos_pg_" << power_gate_pin << " "; 
      fp << "LGND "; 
    }
    fp << tech_lib.transistor_model_name(tech_model, TECH_LIB_TRANSISTOR_NMOS) << TRANSISTOR_WRAPPER_POSTFIX; 
    fp << " W=" << std::setprecision(10) << trans_width;
    fp << "\n";

    /* Cache the last pin*/
    last_en_pin = power_gate_pin;
  }

  fp << "Xnmos_" << trans_name_postfix << " ";
  fp << output_port_name << " "; 
  fp << input_port_name << " "; 
  fp << output_port_name << " _nmos_pg_" << circuit_lib.pins(en_port).back() << " "; 
  fp << "LGND "; 
  fp << tech_lib.transistor_model_name(tech_model, TECH_LIB_TRANSISTOR_NMOS) << TRANSISTOR_WRAPPER_POSTFIX; 
  fp << " W=" << std::setprecision(10) << trans_width;
  fp << "\n";

  return CMD_EXEC_SUCCESS;
}

/********************************************************************
 * Generate the SPICE subckt for a power gated inverter
 * The Enable signal controlled the power gating
 *
 * Note: 
 * - This function supports multi-bit power gating 
 *
 * Schematic
 *            LVDD
 *              |
 *             -
 *   ENb[0] -o||
 *             -
 *              |
 *             -
 *   ENb[1] -o||
 *             -
 *              |
 *
 *            ... <More control signals if available in the port>
 *
 *              |
 *             -
 *        +-o||
 *        |    -
 *        |     |
 *   in-->+     +--> OUT
 *        |     |
 *        |    -
 *        +--||
 *             -
 *
 *            ... <More control signals if available in the port>
 *
 *              |
 *             -
 *    EN[1] -||
 *             -
 *              |
 *             -
 *    EN[0] -||
 *             -
 *              |
 *            LGND
 *
 *******************************************************************/
static 
int print_spice_powergated_inverter_subckt(std::fstream& fp,
                                           const ModuleManager& module_manager,
                                           const ModuleId& module_id,
                                           const CircuitLibrary& circuit_lib,
                                           const CircuitModelId& circuit_model,
                                           const TechnologyLibrary& tech_lib,
                                           const TechnologyModelId& tech_model) {
  if (false == valid_file_stream(fp)) {
    return CMD_EXEC_FATAL_ERROR;
  }

  /* Print the inverter subckt definition */
  print_spice_subckt_definition(fp, module_manager, module_id); 

  /* Find the input and output ports:
   * we do NOT support global ports here, 
   * it should be handled in another type of inverter subckt (power-gated)
   */
  std::vector<CircuitPortId> input_ports = circuit_lib.model_ports_by_type(circuit_model, CIRCUIT_MODEL_PORT_INPUT, true);
  std::vector<CircuitPortId> output_ports = circuit_lib.model_ports_by_type(circuit_model, CIRCUIT_MODEL_PORT_OUTPUT, true);

  /* Make sure:
   * There is only 1 input port and 1 output port, 
   * each size of which is 1
   */
  VTR_ASSERT( (1 == input_ports.size()) && (1 == circuit_lib.port_size(input_ports[0])) );
  VTR_ASSERT( (1 == output_ports.size()) && (1 == circuit_lib.port_size(output_ports[0])) );

  /* If the circuit model is power-gated, we need to find at least one global config_enable signals */
  VTR_ASSERT(true == circuit_lib.is_power_gated(circuit_model));
  CircuitPortId en_port = find_circuit_model_power_gate_en_port(circuit_lib, circuit_model);
  CircuitPortId enb_port = find_circuit_model_power_gate_enb_port(circuit_lib, circuit_model);
  VTR_ASSERT(true == circuit_lib.valid_circuit_port_id(en_port));
  VTR_ASSERT(true == circuit_lib.valid_circuit_port_id(enb_port));

  int status = CMD_EXEC_SUCCESS;

  /* Consider use size/bin to compact layout:
   * Try to size transistors to the max width for each bin
   * The last bin may not reach the max width 
   */
  float regular_pmos_bin_width = tech_lib.transistor_model_max_width(tech_model, TECH_LIB_TRANSISTOR_PMOS);
  float total_pmos_width = circuit_lib.buffer_size(circuit_model)
                         * tech_lib.model_pn_ratio(tech_model)
                         * tech_lib.transistor_model_min_width(tech_model, TECH_LIB_TRANSISTOR_PMOS);
  int num_pmos_bins = std::ceil(total_pmos_width / regular_pmos_bin_width);
  float last_pmos_bin_width = std::fmod(total_pmos_width, regular_pmos_bin_width);
  for (int ibin = 0; ibin < num_pmos_bins; ++ibin) { 
    float curr_bin_width = regular_pmos_bin_width;
    /* For last bin, we need an irregular width */
    if ((ibin == num_pmos_bins - 1) 
       && (0. != last_pmos_bin_width)) {
      curr_bin_width = last_pmos_bin_width;
    }
    status = print_spice_powergated_inverter_pmos_modeling(fp,
                                                           std::to_string(ibin),
                                                           circuit_lib.port_prefix(input_ports[0]), 
                                                           circuit_lib.port_prefix(output_ports[0]), 
                                                           circuit_lib,
                                                           enb_port,
                                                           tech_lib,
                                                           tech_model,
                                                           curr_bin_width);
    if (CMD_EXEC_FATAL_ERROR == status) {
      return status;
    }
  }

  /* Consider use size/bin to compact layout:
   * Try to size transistors to the max width for each bin
   * The last bin may not reach the max width 
   */
  float regular_nmos_bin_width = tech_lib.transistor_model_max_width(tech_model, TECH_LIB_TRANSISTOR_NMOS);
  float total_nmos_width = circuit_lib.buffer_size(circuit_model)
                           * tech_lib.transistor_model_min_width(tech_model, TECH_LIB_TRANSISTOR_NMOS);
  int num_nmos_bins = std::ceil(total_nmos_width / regular_nmos_bin_width);
  float last_nmos_bin_width = std::fmod(total_nmos_width, regular_nmos_bin_width);
  for (int ibin = 0; ibin < num_nmos_bins; ++ibin) { 
    float curr_bin_width = regular_nmos_bin_width;
    /* For last bin, we need an irregular width */
    if ((ibin == num_nmos_bins - 1) 
       && (0. != last_nmos_bin_width)) {
      curr_bin_width = last_nmos_bin_width;
    }

    status = print_spice_powergated_inverter_nmos_modeling(fp,
                                                           std::to_string(ibin),
                                                           circuit_lib.port_prefix(input_ports[0]), 
                                                           circuit_lib.port_prefix(output_ports[0]), 
                                                           circuit_lib,
                                                           en_port,
                                                           tech_lib,
                                                           tech_model,
                                                           curr_bin_width);
    if (CMD_EXEC_FATAL_ERROR == status) {
      return status;
    }
  }

  print_spice_subckt_end(fp, module_manager.module_name(module_id)); 

  return CMD_EXEC_SUCCESS;
}

/********************************************************************
 * Generate the SPICE modeling for the PMOS part of a regular inverter
 *
 * This function is created to be shared by inverter and buffer SPICE netlist writer
 *
 * Note: 
 * - This function does NOT create a file
 *   but requires a file stream created
 * - This function only output SPICE modeling for 
 *   an inverter. Any preprocessing or subckt definition should not be included!
 *******************************************************************/
static 
int print_spice_regular_inverter_pmos_modeling(std::fstream& fp,
                                               const std::string& trans_name_postfix,
                                               const std::string& input_port_name,
                                               const std::string& output_port_name,
                                               const TechnologyLibrary& tech_lib,
                                               const TechnologyModelId& tech_model,
                                               const float& trans_width) {

  if (false == valid_file_stream(fp)) {
    return CMD_EXEC_FATAL_ERROR;
  }

  /* Write transistor pairs using the technology model */
  fp << "Xpmos_" << trans_name_postfix << " ";
  fp << output_port_name << " "; 
  fp << input_port_name << " "; 
  fp << "LVDD "; 
  fp << "LVDD "; 
  fp << tech_lib.transistor_model_name(tech_model, TECH_LIB_TRANSISTOR_PMOS) << TRANSISTOR_WRAPPER_POSTFIX; 
  fp << " W=" << std::setprecision(10) << trans_width;
  fp << "\n";

  return CMD_EXEC_SUCCESS;
}

/********************************************************************
 * Generate the SPICE modeling for the NMOS part of a regular inverter
 *
 * This function is created to be shared by inverter and buffer SPICE netlist writer
 *
 * Note: 
 * - This function does NOT create a file
 *   but requires a file stream created
 * - This function only output SPICE modeling for 
 *   an inverter. Any preprocessing or subckt definition should not be included!
 *******************************************************************/
static 
int print_spice_regular_inverter_nmos_modeling(std::fstream& fp,
                                               const std::string& trans_name_postfix,
                                               const std::string& input_port_name,
                                               const std::string& output_port_name,
                                               const TechnologyLibrary& tech_lib,
                                               const TechnologyModelId& tech_model,
                                               const float& trans_width) {

  if (false == valid_file_stream(fp)) {
    return CMD_EXEC_FATAL_ERROR;
  }

  fp << "Xnmos_" << trans_name_postfix << " ";
  fp << output_port_name << " "; 
  fp << input_port_name << " "; 
  fp << "LGND "; 
  fp << "LGND "; 
  fp << tech_lib.transistor_model_name(tech_model, TECH_LIB_TRANSISTOR_NMOS) << TRANSISTOR_WRAPPER_POSTFIX; 
  fp << " W=" << std::setprecision(10) << trans_width;
  fp << "\n";

  return CMD_EXEC_SUCCESS;
}

/********************************************************************
 * Generate the SPICE subckt for a regular inverter
 *
 * Note: 
 * - This function does NOT support power-gating
 *   It should be managed in a separated function
 *
 * Schematic
 *          LVDD
 *            |
 *           -
 *      +-o||
 *      |    -
 *      |     |
 * in-->+     +--> OUT
 *      |     |
 *      |    -
 *      +--||
 *           -
 *            |
 *          LGND
 *
 *******************************************************************/
static 
int print_spice_regular_inverter_subckt(std::fstream& fp,
                                        const ModuleManager& module_manager,
                                        const ModuleId& module_id,
                                        const CircuitLibrary& circuit_lib,
                                        const CircuitModelId& circuit_model,
                                        const TechnologyLibrary& tech_lib,
                                        const TechnologyModelId& tech_model) {
  if (false == valid_file_stream(fp)) {
    return CMD_EXEC_FATAL_ERROR;
  }

  /* Print the inverter subckt definition */
  print_spice_subckt_definition(fp, module_manager, module_id); 

  /* Find the input and output ports:
   * we do NOT support global ports here, 
   * it should be handled in another type of inverter subckt (power-gated)
   */
  std::vector<CircuitPortId> input_ports = circuit_lib.model_ports_by_type(circuit_model, CIRCUIT_MODEL_PORT_INPUT, true);
  std::vector<CircuitPortId> output_ports = circuit_lib.model_ports_by_type(circuit_model, CIRCUIT_MODEL_PORT_OUTPUT, true);

  /* Make sure:
   * There is only 1 input port and 1 output port, 
   * each size of which is 1
   */
  VTR_ASSERT( (1 == input_ports.size()) && (1 == circuit_lib.port_size(input_ports[0])) );
  VTR_ASSERT( (1 == output_ports.size()) && (1 == circuit_lib.port_size(output_ports[0])) );

  int status = CMD_EXEC_SUCCESS;

  /* Consider use size/bin to compact layout:
   * Try to size transistors to the max width for each bin
   * The last bin may not reach the max width 
   */
  float regular_pmos_bin_width = tech_lib.transistor_model_max_width(tech_model, TECH_LIB_TRANSISTOR_PMOS);
  float total_pmos_width = circuit_lib.buffer_size(circuit_model)
                           * tech_lib.model_pn_ratio(tech_model)
                           * tech_lib.transistor_model_min_width(tech_model, TECH_LIB_TRANSISTOR_PMOS);
  int num_pmos_bins = std::ceil(total_pmos_width / regular_pmos_bin_width);
  float last_pmos_bin_width = std::fmod(total_pmos_width, regular_pmos_bin_width);
  for (int ibin = 0; ibin < num_pmos_bins; ++ibin) { 
    float curr_bin_width = regular_pmos_bin_width;
    /* For last bin, we need an irregular width */
    if ((ibin == num_pmos_bins - 1) 
       && (0. != last_pmos_bin_width)) {
      curr_bin_width = last_pmos_bin_width;
    }

    status = print_spice_regular_inverter_pmos_modeling(fp,
                                                        std::to_string(ibin),
                                                        circuit_lib.port_prefix(input_ports[0]), 
                                                        circuit_lib.port_prefix(output_ports[0]), 
                                                        tech_lib,
                                                        tech_model,
                                                        curr_bin_width);
    if (CMD_EXEC_FATAL_ERROR == status) {
      return status;
    }
  }

  /* Consider use size/bin to compact layout:
   * Try to size transistors to the max width for each bin
   * The last bin may not reach the max width 
   */
  float regular_nmos_bin_width = tech_lib.transistor_model_max_width(tech_model, TECH_LIB_TRANSISTOR_NMOS);
  float total_nmos_width = circuit_lib.buffer_size(circuit_model)
                           * tech_lib.transistor_model_min_width(tech_model, TECH_LIB_TRANSISTOR_NMOS);
  int num_nmos_bins = std::ceil(total_nmos_width / regular_nmos_bin_width);
  float last_nmos_bin_width = std::fmod(total_nmos_width, regular_nmos_bin_width);

  for (int ibin = 0; ibin < num_nmos_bins; ++ibin) { 
    float curr_bin_width = regular_nmos_bin_width;
    /* For last bin, we need an irregular width */
    if ((ibin == num_nmos_bins - 1) 
       && (0. != last_nmos_bin_width)) {
      curr_bin_width = last_nmos_bin_width;
    }

    status = print_spice_regular_inverter_nmos_modeling(fp,
                                                        std::to_string(ibin),
                                                        circuit_lib.port_prefix(input_ports[0]), 
                                                        circuit_lib.port_prefix(output_ports[0]), 
                                                        tech_lib,
                                                        tech_model,
                                                        curr_bin_width);
    if (CMD_EXEC_FATAL_ERROR == status) {
      return status;
    }
  }

  print_spice_subckt_end(fp, module_manager.module_name(module_id)); 

  return status;
}

/********************************************************************
 * Generate the SPICE subckt for an inverter
 * Branch on the different circuit topologies
 *******************************************************************/
static 
int print_spice_inverter_subckt(std::fstream& fp,
                                const ModuleManager& module_manager,
                                const ModuleId& module_id,
                                const CircuitLibrary& circuit_lib,
                                const CircuitModelId& circuit_model,
                                const TechnologyLibrary& tech_lib,
                                const TechnologyModelId& tech_model) {
  int status = CMD_EXEC_SUCCESS;
  if (true == circuit_lib.is_power_gated(circuit_model)) {
    status = print_spice_powergated_inverter_subckt(fp,
                                                    module_manager, module_id,
                                                    circuit_lib, circuit_model,
                                                    tech_lib, tech_model);
  } else { 
    VTR_ASSERT_SAFE(false == circuit_lib.is_power_gated(circuit_model));
    status = print_spice_regular_inverter_subckt(fp,
                                                 module_manager, module_id,
                                                 circuit_lib, circuit_model,
                                                 tech_lib, tech_model);
  }
 
  return status;
}

/********************************************************************
 * Generate the SPICE subckt for a power-gated buffer
 * which contains at least 2 stages
 *
 * Schematic of a multi-stage buffer
 * 
 *            LVDD              LVDD
 *              |                |
 *             -                -
 *   ENb[0] -o||     ENb[0] -o||
 *             -                -
 *              |                |
 *             -                -
 *   ENb[1] -o||     ENb[1] -o||
 *             -                -
 *              |                |
 *
 *            ... <More control signals if available in the port>
 *
 *              |                |
 *             -                -
 *        +-o||            +-o||
 *        |    -           |    -
 *        |     |          |     |
 *   in-->+     +-- ... ---+---->+---> out
 *        |     |          |     |
 *        |    -           |    -
 *        +--||            +--||
 *             -                -
 *              |                |
 *
 *            ... <More control signals if available in the port>
 *
 *              |                |
 *             -                -
 *   EN[0]  -||       EN[0]  -||
 *             -                -
 *              |                |
 *             -                -
 *   EN[1]  -||       EN[1]  -||
 *             -                -
 *              |                |

 *              |                |
 *            LGND             LGND
 *
 *******************************************************************/
static 
int print_spice_powergated_buffer_subckt(std::fstream& fp,
                                         const ModuleManager& module_manager,
                                         const ModuleId& module_id,
                                         const CircuitLibrary& circuit_lib,
                                         const CircuitModelId& circuit_model,
                                         const TechnologyLibrary& tech_lib,
                                         const TechnologyModelId& tech_model) {
  if (false == valid_file_stream(fp)) {
    return CMD_EXEC_FATAL_ERROR;
  }

  /* Print the inverter subckt definition */
  print_spice_subckt_definition(fp, module_manager, module_id); 

  /* Find the input and output ports:
   * we do NOT support global ports here, 
   * it should be handled in another type of inverter subckt (power-gated)
   */
  std::vector<CircuitPortId> input_ports = circuit_lib.model_ports_by_type(circuit_model, CIRCUIT_MODEL_PORT_INPUT, true);
  std::vector<CircuitPortId> output_ports = circuit_lib.model_ports_by_type(circuit_model, CIRCUIT_MODEL_PORT_OUTPUT, true);

  /* Make sure:
   * There is only 1 input port and 1 output port, 
   * each size of which is 1
   */
  VTR_ASSERT( (1 == input_ports.size()) && (1 == circuit_lib.port_size(input_ports[0])) );
  VTR_ASSERT( (1 == output_ports.size()) && (1 == circuit_lib.port_size(output_ports[0])) );

  /* If the circuit model is power-gated, we need to find at least one global config_enable signals */
  VTR_ASSERT(true == circuit_lib.is_power_gated(circuit_model));
  CircuitPortId en_port = find_circuit_model_power_gate_en_port(circuit_lib, circuit_model);
  CircuitPortId enb_port = find_circuit_model_power_gate_enb_port(circuit_lib, circuit_model);
  VTR_ASSERT(true == circuit_lib.valid_circuit_port_id(en_port));
  VTR_ASSERT(true == circuit_lib.valid_circuit_port_id(enb_port));

  int status = CMD_EXEC_SUCCESS;

  /* Buffers must have >= 2 stages */
  VTR_ASSERT(2 <= circuit_lib.buffer_num_levels(circuit_model));

  /* Build the array denoting width of inverters per stage */
  std::vector<float> buffer_widths(circuit_lib.buffer_num_levels(circuit_model), 1);
  for (size_t level = 0; level < circuit_lib.buffer_num_levels(circuit_model); ++level) {
    buffer_widths[level] = circuit_lib.buffer_size(circuit_model)
                         * std::pow(circuit_lib.buffer_f_per_stage(circuit_model), level);
  }

  for (size_t level = 0; level < circuit_lib.buffer_num_levels(circuit_model); ++level) {
    std::string input_port_name = circuit_lib.port_prefix(input_ports[0]); 
    std::string output_port_name = circuit_lib.port_prefix(output_ports[0]); 

    /* Special for first stage: output port should be an intermediate node 
     * Special for rest of stages: input port should be the output of previous stage
     */
    if (0 == level) {
      output_port_name += std::string("_level") + std::to_string(level);
    } else {
      VTR_ASSERT(0 < level);
      input_port_name += std::string("_level") + std::to_string(level - 1);
    }

    /* Consider use size/bin to compact layout:
     * Try to size transistors to the max width for each bin
     * The last bin may not reach the max width 
     */
    float regular_pmos_bin_width = tech_lib.transistor_model_max_width(tech_model, TECH_LIB_TRANSISTOR_PMOS);
    float total_pmos_width = buffer_widths[level]
                             * tech_lib.model_pn_ratio(tech_model)
                             * tech_lib.transistor_model_min_width(tech_model, TECH_LIB_TRANSISTOR_PMOS);
    int num_pmos_bins = std::ceil(total_pmos_width / regular_pmos_bin_width);
    float last_pmos_bin_width = std::fmod(total_pmos_width, regular_pmos_bin_width);

    for (int ibin = 0; ibin < num_pmos_bins; ++ibin) { 
      float curr_bin_width = regular_pmos_bin_width;
      /* For last bin, we need an irregular width */
      if ((ibin == num_pmos_bins - 1) 
         && (0. != last_pmos_bin_width)) {
        curr_bin_width = last_pmos_bin_width;
      }

      std::string name_postfix = std::string("level") + std::to_string(level) + std::string("_bin") + std::to_string(ibin);

      status = print_spice_powergated_inverter_pmos_modeling(fp,
                                                             name_postfix,
                                                             circuit_lib.port_prefix(input_ports[0]), 
                                                             circuit_lib.port_prefix(output_ports[0]), 
                                                             circuit_lib,
                                                             enb_port,
                                                             tech_lib,
                                                             tech_model,
                                                             curr_bin_width);
      if (CMD_EXEC_FATAL_ERROR == status) {
        return status;
      }
    }

    /* Consider use size/bin to compact layout:
     * Try to size transistors to the max width for each bin
     * The last bin may not reach the max width 
     */
    float regular_nmos_bin_width = tech_lib.transistor_model_max_width(tech_model, TECH_LIB_TRANSISTOR_NMOS);
    float total_nmos_width = buffer_widths[level]
                             * tech_lib.transistor_model_min_width(tech_model, TECH_LIB_TRANSISTOR_NMOS);
    int num_nmos_bins = std::ceil(total_nmos_width / regular_nmos_bin_width);
    float last_nmos_bin_width = std::fmod(total_nmos_width, regular_nmos_bin_width);

    for (int ibin = 0; ibin < num_nmos_bins; ++ibin) { 
      float curr_bin_width = regular_nmos_bin_width;
      /* For last bin, we need an irregular width */
      if ((ibin == num_nmos_bins - 1) 
         && (0. != last_nmos_bin_width)) {
        curr_bin_width = last_nmos_bin_width;
      }

      std::string name_postfix = std::string("level") + std::to_string(level) + std::string("_bin") + std::to_string(ibin);

      status = print_spice_powergated_inverter_nmos_modeling(fp,
                                                             name_postfix,
                                                             circuit_lib.port_prefix(input_ports[0]), 
                                                             circuit_lib.port_prefix(output_ports[0]), 
                                                             circuit_lib,
                                                             en_port,
                                                             tech_lib,
                                                             tech_model,
                                                             curr_bin_width);
      if (CMD_EXEC_FATAL_ERROR == status) {
        return status;
      }
    }
  }

  print_spice_subckt_end(fp, module_manager.module_name(module_id)); 

  return CMD_EXEC_SUCCESS;
}


/********************************************************************
 * Generate the SPICE subckt for a regular buffer
 * which contains at least 2 stages
 *
 * Note: 
 * - This function does NOT support power-gating
 *   It should be managed in a separated function
 *
 * Schematic of a multi-stage buffer
 * 
 *          LVDD              LVDD
 *            |                |
 *           -                -
 *      +-o||            +-o||
 *      |    -           |    -
 *      |     |          |     |
 * in-->+     +-- ... ---+---->+---> out
 *      |     |          |     |
 *      |    -           |    -
 *      +--||            +--||
 *           -                -
 *            |                |
 *          LGND             LGND
 *
 *******************************************************************/
static 
int print_spice_regular_buffer_subckt(std::fstream& fp,
                                      const ModuleManager& module_manager,
                                      const ModuleId& module_id,
                                      const CircuitLibrary& circuit_lib,
                                      const CircuitModelId& circuit_model,
                                      const TechnologyLibrary& tech_lib,
                                      const TechnologyModelId& tech_model) {
  if (false == valid_file_stream(fp)) {
    return CMD_EXEC_FATAL_ERROR;
  }

  /* Print the inverter subckt definition */
  print_spice_subckt_definition(fp, module_manager, module_id); 

  /* Find the input and output ports:
   * we do NOT support global ports here, 
   * it should be handled in another type of inverter subckt (power-gated)
   */
  std::vector<CircuitPortId> input_ports = circuit_lib.model_ports_by_type(circuit_model, CIRCUIT_MODEL_PORT_INPUT, true);
  std::vector<CircuitPortId> output_ports = circuit_lib.model_ports_by_type(circuit_model, CIRCUIT_MODEL_PORT_OUTPUT, true);

  /* Make sure:
   * There is only 1 input port and 1 output port, 
   * each size of which is 1
   */
  VTR_ASSERT( (1 == input_ports.size()) && (1 == circuit_lib.port_size(input_ports[0])) );
  VTR_ASSERT( (1 == output_ports.size()) && (1 == circuit_lib.port_size(output_ports[0])) );

  int status = CMD_EXEC_SUCCESS;

  /* Buffers must have >= 2 stages */
  VTR_ASSERT(2 <= circuit_lib.buffer_num_levels(circuit_model));

  /* Build the array denoting width of inverters per stage */
  std::vector<float> buffer_widths(circuit_lib.buffer_num_levels(circuit_model), 1);
  for (size_t level = 0; level < circuit_lib.buffer_num_levels(circuit_model); ++level) {
    buffer_widths[level] = circuit_lib.buffer_size(circuit_model)
                         * std::pow(circuit_lib.buffer_f_per_stage(circuit_model), level);
  }

  for (size_t level = 0; level < circuit_lib.buffer_num_levels(circuit_model); ++level) {
    std::string input_port_name = circuit_lib.port_prefix(input_ports[0]); 
    std::string output_port_name = circuit_lib.port_prefix(output_ports[0]); 

    /* Special for first stage: output port should be an intermediate node 
     * Special for rest of stages: input port should be the output of previous stage
     */
    if (0 == level) {
      output_port_name += std::string("_level") + std::to_string(level);
    } else {
      VTR_ASSERT(0 < level);
      input_port_name += std::string("_level") + std::to_string(level - 1);
    }
    
    /* Consider use size/bin to compact layout:
     * Try to size transistors to the max width for each bin
     * The last bin may not reach the max width 
     */
    float regular_pmos_bin_width = tech_lib.transistor_model_max_width(tech_model, TECH_LIB_TRANSISTOR_PMOS);
    float total_pmos_width = buffer_widths[level]
                             * tech_lib.model_pn_ratio(tech_model)
                             * tech_lib.transistor_model_min_width(tech_model, TECH_LIB_TRANSISTOR_PMOS);
    int num_pmos_bins = std::ceil(total_pmos_width / regular_pmos_bin_width);
    float last_pmos_bin_width = std::fmod(total_pmos_width, regular_pmos_bin_width);

    for (int ibin = 0; ibin < num_pmos_bins; ++ibin) { 
      float curr_bin_width = regular_pmos_bin_width;
      /* For last bin, we need an irregular width */
      if ((ibin == num_pmos_bins - 1) 
         && (0. != last_pmos_bin_width)) {
        curr_bin_width = last_pmos_bin_width;
      }

      std::string name_postfix = std::string("level") + std::to_string(level) + std::string("_bin") + std::to_string(ibin);

      status = print_spice_regular_inverter_pmos_modeling(fp,
                                                          name_postfix,
                                                          circuit_lib.port_prefix(input_ports[0]), 
                                                          circuit_lib.port_prefix(output_ports[0]), 
                                                          tech_lib,
                                                          tech_model,
                                                          curr_bin_width);
      if (CMD_EXEC_FATAL_ERROR == status) {
        return status;
      }
    }

    /* Consider use size/bin to compact layout:
     * Try to size transistors to the max width for each bin
     * The last bin may not reach the max width 
     */
    float regular_nmos_bin_width = tech_lib.transistor_model_max_width(tech_model, TECH_LIB_TRANSISTOR_NMOS);
    float total_nmos_width = buffer_widths[level]
                              * tech_lib.transistor_model_min_width(tech_model, TECH_LIB_TRANSISTOR_NMOS);
    int num_nmos_bins = std::ceil(total_nmos_width / regular_nmos_bin_width);
    float last_nmos_bin_width = std::fmod(total_nmos_width, regular_nmos_bin_width);

    for (int ibin = 0; ibin < num_nmos_bins; ++ibin) { 
      float curr_bin_width = regular_nmos_bin_width;
      /* For last bin, we need an irregular width */
      if ((ibin == num_nmos_bins - 1) 
         && (0. != last_nmos_bin_width)) {
        curr_bin_width = last_nmos_bin_width;
      }

      std::string name_postfix = std::string("level") + std::to_string(level) + std::string("_bin") + std::to_string(ibin);

      status = print_spice_regular_inverter_nmos_modeling(fp,
                                                          name_postfix,
                                                          circuit_lib.port_prefix(input_ports[0]), 
                                                          circuit_lib.port_prefix(output_ports[0]), 
                                                          tech_lib,
                                                          tech_model,
                                                          curr_bin_width);
      if (CMD_EXEC_FATAL_ERROR == status) {
        return status;
      }
    }
  }

  print_spice_subckt_end(fp, module_manager.module_name(module_id)); 

  return status;
}

/********************************************************************
 * Generate the SPICE subckt for an buffer 
 * which consists of multiple stage of inverters
 *******************************************************************/
static 
int print_spice_buffer_subckt(std::fstream& fp,
                              const ModuleManager& module_manager,
                              const ModuleId& module_id,
                              const CircuitLibrary& circuit_lib,
                              const CircuitModelId& circuit_model,
                              const TechnologyLibrary& tech_lib,
                              const TechnologyModelId& tech_model) {
  int status = CMD_EXEC_SUCCESS;
  if (true == circuit_lib.is_power_gated(circuit_model)) {
    status = print_spice_powergated_buffer_subckt(fp,
                                                  module_manager, module_id,
                                                  circuit_lib, circuit_model,
                                                  tech_lib, tech_model);
  } else { 
    VTR_ASSERT_SAFE(false == circuit_lib.is_power_gated(circuit_model));
    status = print_spice_regular_buffer_subckt(fp,
                                               module_manager, module_id,
                                               circuit_lib, circuit_model,
                                               tech_lib, tech_model);
  }
 
  return status;
}

/********************************************************************
 * Generate the SPICE netlist for essential gates:
 * - inverters and their templates
 * - buffers and their templates
 * - pass-transistor or transmission gates
 * - logic gates
 *******************************************************************/
int print_spice_essential_gates(NetlistManager& netlist_manager,
                                const ModuleManager& module_manager,
                                const CircuitLibrary& circuit_lib,
                                const TechnologyLibrary& tech_lib,
                                const std::map<CircuitModelId, TechnologyModelId>& circuit_tech_binding,
                                const std::string& submodule_dir) {
  std::string spice_fname = submodule_dir + std::string(ESSENTIALS_SPICE_FILE_NAME);

  std::fstream fp;

  /* Create the file stream */
  fp.open(spice_fname, std::fstream::out | std::fstream::trunc);
  /* Check if the file stream if valid or not */
  check_file_stream(spice_fname.c_str(), fp); 

  /* Create file */
  VTR_LOG("Generating SPICE netlist '%s' for essential gates...",
          spice_fname.c_str()); 

  print_spice_file_header(fp, std::string("Essential gates"));

  int status = CMD_EXEC_SUCCESS;

  /* Iterate over the circuit models */
  for (const CircuitModelId& circuit_model : circuit_lib.models()) {
    /* Bypass models require extern netlists */
    if (!circuit_lib.model_circuit_netlist(circuit_model).empty()) {
      continue;
    }

    /* Spot module id */
    const ModuleId& module_id = module_manager.find_module(circuit_lib.model_name(circuit_model));

    TechnologyModelId tech_model; 
    /* Focus on inverter/buffer/pass-gate/logic gates only */
    if ( (CIRCUIT_MODEL_INVBUF == circuit_lib.model_type(circuit_model))
      || (CIRCUIT_MODEL_PASSGATE == circuit_lib.model_type(circuit_model))
      || (CIRCUIT_MODEL_GATE == circuit_lib.model_type(circuit_model))) {
      auto result = circuit_tech_binding.find(circuit_model);
      if (result == circuit_tech_binding.end()) {
        VTR_LOGF_ERROR(__FILE__, __LINE__,
                       "Unable to find technology binding for circuit model '%s'!\n",
                       circuit_lib.model_name(circuit_model).c_str()); 
        return CMD_EXEC_FATAL_ERROR;
      }
      /* Valid technology binding. Assign techology model */
      tech_model = result->second;
      /* Ensure we have a valid technology model */
      VTR_ASSERT(true == tech_lib.valid_model_id(tech_model));
      VTR_ASSERT(TECH_LIB_MODEL_TRANSISTOR == tech_lib.model_type(tech_model));
    }

    /* Now branch on netlist writing */
    if (CIRCUIT_MODEL_INVBUF == circuit_lib.model_type(circuit_model)) {
      if (CIRCUIT_MODEL_BUF_INV == circuit_lib.buffer_type(circuit_model)) {
        VTR_ASSERT(true == module_manager.valid_module_id(module_id));
        status = print_spice_inverter_subckt(fp,
                                             module_manager, module_id,
                                             circuit_lib, circuit_model,
                                             tech_lib, tech_model);
      } else {
        VTR_ASSERT(CIRCUIT_MODEL_BUF_BUF == circuit_lib.buffer_type(circuit_model));
        status = print_spice_buffer_subckt(fp,
                                           module_manager, module_id,
                                           circuit_lib, circuit_model,
                                           tech_lib, tech_model);
      }

      if (CMD_EXEC_FATAL_ERROR == status) {
        break;
      }

      /* Finish, go to the next */
      continue;
    }
  } 

  /* Close file handler*/
  fp.close();

  /* Add fname to the netlist name list */
  NetlistId nlist_id = netlist_manager.add_netlist(spice_fname);
  VTR_ASSERT(NetlistId::INVALID() != nlist_id);
  netlist_manager.set_netlist_type(nlist_id, NetlistManager::SUBMODULE_NETLIST);

  VTR_LOG("Done\n");

  return status;
}

} /* end namespace openfpga */
