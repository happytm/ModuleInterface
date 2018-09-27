#pragma once

#include <MI/ModuleInterfaceSet.h>
#include <MI_PJON/PJONModuleInterface.h>

typedef void (*mis_receive_function)(const uint8_t *payload, uint16_t length, const PJON_Packet_Info &packet_info, const ModuleInterface *module_interface);

void mis_global_receive_function(uint8_t *payload, uint16_t length, const PJON_Packet_Info &packet_info);

class PJONModuleInterfaceSet : public ModuleInterfaceSet {
protected:
  uint32_t last_time_sync = 0;
  Link *pjon = NULL;
  mis_receive_function custom_receive_function = NULL;
  friend void mis_global_receive_function(uint8_t *payload, uint16_t length, const PJON_Packet_Info &packet_info);
public:
  PJONModuleInterfaceSet(const char *prefix = NULL) : ModuleInterfaceSet(prefix) { init(); }
  PJONModuleInterfaceSet(Link &bus, const uint8_t num_interfaces, const char *prefix = NULL) : ModuleInterfaceSet(prefix) {
    init(); 
    this->num_interfaces = num_interfaces; 
    if (num_interfaces > 0) {
      interfaces = new ModuleInterface*[num_interfaces];
      for (uint8_t i = 0; i < num_interfaces; i++) interfaces[i] = new PJONModuleInterface();
    }
    pjon = &bus;
    pjon->set_receiver(mis_global_receive_function, this);
  }
  // Specifying modules as textual list like "BlinkModule:bm:44 TestModule:tm:44:0.0.0.1":
  PJONModuleInterfaceSet(Link &bus, const char *interface_list, const char *prefix = NULL) : ModuleInterfaceSet(prefix) {
    init();
    pjon = &bus;
    pjon->set_receiver(mis_global_receive_function, this);
    if (interface_list) set_interface_list(interface_list);
  }
  void init() { }
  void set_interface_list(const char *interface_list) {
    // This function can only be called once after startup
    if (num_interfaces > 0) return;

    // Count number of interfaces
    const char *p = interface_list;
    while (*p != 0) {
      p++; 
      if (*p == 0 || *p == ' ') num_interfaces++;
      while (*p == ' ') p++; // Allow multiple spaces in sequence
    }

    // Allocate
    interfaces = new ModuleInterface*[num_interfaces];
    for (uint8_t i = 0; i < num_interfaces; i++) interfaces[i] = new PJONModuleInterface();

    // Set names and ids
    p = interface_list;
    uint8_t cnt = 0;
    #ifdef DEBUG_PRINT
    DPRINT("Interface count="); DPRINT(num_interfaces); DPRINT(": ");
    #endif
    while (*p != 0 && cnt < num_interfaces) {
      ((PJONModuleInterface*) (interfaces[cnt]))->set_name_prefix_and_address(p);
      ((PJONModuleInterface*) (interfaces[cnt]))->set_bus(*pjon);
      #ifdef DEBUG_PRINT
      if (cnt>0) DPRINT(", "); DPRINT(((PJONModuleInterface*) (interfaces[cnt]))->module_name);
      #endif
      cnt++;
      while (*p != 0 && *p != ' ') p++;
      while (*p == ' ') p++; // Find char after spaces
    }
    #ifdef DEBUG_PRINT
    DPRINTLN("");
    #endif
  }
  Link *get_link() { return pjon; }

  void set_receiver(mis_receive_function r) {
    pjon->set_receiver(mis_global_receive_function, this); // Make sure main receiver function is registered
    custom_receive_function = r; // Register custom/user callback function to receive non-ModuleInterface related messages
  }

  void update_contracts() { for (uint8_t i = 0; i < num_interfaces; i++) ((PJONModuleInterface*) (interfaces[i]))->update_contract(sampling_time_outputs); }
  void update_values() { for (uint8_t i = 0; i < num_interfaces; i++) ((PJONModuleInterface*) (interfaces[i]))->update_values(sampling_time_outputs); }
  void update_settings() { for (uint8_t i = 0; i < num_interfaces; i++) ((PJONModuleInterface*) (interfaces[i]))->update_settings(sampling_time_settings); }
  void update_statuses() { for (uint8_t i = 0; i < num_interfaces; i++) ((PJONModuleInterface*) (interfaces[i]))->update_status(sampling_time_outputs); }
  void send_settings() { for (uint8_t i = 0; i < num_interfaces; i++) ((PJONModuleInterface*) (interfaces[i]))->send_settings(); }
  void send_inputs() { for (uint8_t i = 0; i < num_interfaces; i++) ((PJONModuleInterface*) (interfaces[i]))->send_inputs(); }
  void send_input_events() { for (uint8_t i = 0; i < num_interfaces; i++) ((PJONModuleInterface*) (interfaces[i]))->send_input_events(); }

  void clear_output_events() { for (uint8_t i = 0; i < num_interfaces; i++) ((PJONModuleInterface*) (interfaces[i]))->outputs.clear_events(); }
  void clear_input_events() { for (uint8_t i = 0; i < num_interfaces; i++) ((PJONModuleInterface*) (interfaces[i]))->inputs.clear_events(); }

  void handle_events() {
    if (!updated_intermodule_dependencies || !got_all_contracts()) return;
    transfer_events_from_outputs_to_inputs();
    send_input_events();
    clear_output_events();
    clear_input_events();
  }

  // Time will be broadcast to all modules unless NO_TIME_SYNC is defined or master itself is not timesynced
  #ifndef NO_TIME_SYNC
  void broadcast_time() {
    if (miTime::IsSynced()) {
      bool scheduled_sync = ((uint32_t)(millis() - last_time_sync) >= 60000); // Time for a scheduled broadcast?
      #ifdef DEBUG_PRINT
        if (scheduled_sync) {DPRINT(F("Scheduled broadcast of time sync: ")); DPRINTLN(miTime::Get()); }
      #endif
      
      // Check if any local bus module has reported that is it missing time
      bool broadcast = scheduled_sync;
      if (!scheduled_sync) {
        for (uint8_t i = 0; i < num_interfaces; i++) { 
          if ((interfaces[i]->status_bits & MISSING_TIME) && has_local_bus(i)) {
            // Same bus, can do broadcast
            broadcast = true;
            #ifdef DEBUG_PRINT
            if (!scheduled_sync) {
              DPRINT(F("Module ")); DPRINT(interfaces[i]->module_name);
              DPRINT(F(" missing time, broadcasting: ")); DPRINTLN(miTime::Get());
            }
            #endif
          }
        }
      }

      // Do the broadcast on local bus
      if (broadcast) {
        send_timesync(0, pjon->get_bus_id());

        // Clear time-missing bit to avoid this triggering continuous broadcasts.
        // If a module did not pick up the broadcast, we will get this information in the next status reply.
        for (uint8_t i = 0; i < num_interfaces; i++) 
          if (has_local_bus(i)) interfaces[i]->status_bits &= ~MISSING_TIME;
      }
      
      // Broadcast will not reach devices on other buses, so send directed time sync to each
      for (uint8_t i = 0; i < num_interfaces; i++) { 
        if ((scheduled_sync || interfaces[i]->status_bits & MISSING_TIME) && !has_local_bus(i)) {
          send_timesync(((PJONModuleInterface*)interfaces[i])->remote_id, ((PJONModuleInterface*)interfaces[i])->remote_bus_id);
          #ifdef DEBUG_PRINT
            if (interfaces[i]->status_bits & MISSING_TIME) DPRINT(F("Module missing time. ")); 
            DPRINT(F("Sending directed sync to "));DPRINT(interfaces[i]->module_name);
            DPRINT(F(": ")); DPRINTLN(miTime::Get());
          #endif

          // Clear time-missing bit to avoid this triggering continuous time sync to this device
          // If a module did not pick up the sync, we will get this information in the next status reply.
          interfaces[i]->status_bits &= ~MISSING_TIME;  
        }
      }
      if (scheduled_sync) last_time_sync = millis();      
    }
  }
  
  bool has_local_bus(uint8_t interface_ix) {
    // Returns true if device is on the same bus as me (the master). Always returns true in local mode.
    return (memcmp(((PJONModuleInterface*)interfaces[interface_ix])->remote_bus_id, pjon->get_bus_id(), 4)==0);
  }
  
  void send_timesync(const uint8_t id, const uint8_t bus_id[]) {
    char buf[5];
    buf[0] = (char) mcSetTime;
    uint32_t t = miTime::Get();
    memcpy(&buf[1], &t, 4);
    pjon->send_packet(id, bus_id, buf, 5, MI_REDUCED_SEND_TIMEOUT);
  }
  #endif
  
  void update() {
    // Do PJON send and receive
    pjon->update();
    pjon->receive(100);

    // Handle incoming events
    handle_events();

    // Broadcast time to all modules with a few minutes interval
    #ifndef NO_TIME_SYNC
    broadcast_time();
    #endif

    // Request the contract of each module if not received already
    update_contracts();

    // Send settings to each module
    if (mi_interval_elapsed(last_settings_sent, sampling_time_settings)) send_settings();

    // Get potential modified settings from each module
    update_settings();

    // Get fresh output values from each module
    update_values();

    // Deliver inputs to each module
    if (mi_interval_elapsed(last_inputs_sent, sampling_time_outputs)) {
      // Transfer outputs from modules to inputs of other modules
      transfer_outputs_to_inputs();

      send_inputs();
    }

    // Get fresh status from each module
    update_statuses();
  }

  uint8_t locate_module(const uint8_t device_id, const uint8_t *bus_id) const {
    uint8_t ix = NO_MODULE;
    for (uint8_t i=0; i<num_interfaces; i++) {
      if (((PJONModuleInterface*) interfaces[i])->remote_id == device_id &&
          (memcmp(((PJONModuleInterface*) interfaces[i])->remote_bus_id, bus_id, 4) == 0)) {
        ix = i; break;
      }
    }
    return ix;
  }

  bool handle_message(const uint8_t *payload, const uint16_t length, const PJON_Packet_Info &packet_info) {
    // Locate the relevant module based on packet info (device id and bus id)
    uint8_t ix = locate_module(packet_info.sender_id, packet_info.sender_bus_id);
    if (ix == NO_MODULE) return false;

    // Let the interface handle the message
    return ((PJONModuleInterface*) interfaces[ix])->handle_message(payload, length, packet_info);
  }

  // These settings specify how often to transfer settings and outputs
  uint16_t sampling_time_settings = 10000,
           sampling_time_outputs = 10000;
  uint32_t last_settings_sent = 0,
           last_inputs_sent = 0;
};

// PJON receive callback function
void mis_global_receive_function(uint8_t *payload, uint16_t length, const PJON_Packet_Info &packet_info) {
  if (packet_info.custom_pointer) {
    PJONModuleInterfaceSet *mis = (PJONModuleInterfaceSet*) packet_info.custom_pointer;

    // Find out which module is sending
    uint8_t ix = mis->locate_module(packet_info.sender_id, packet_info.sender_bus_id);
    PJONModuleInterface *interface = NULL;
    if (ix != NO_MODULE) {
      interface = (PJONModuleInterface*) mis->interfaces[ix];

      // Let interface handle ModuleInterface related message
      if (interface->handle_message(payload, length, packet_info)) return;
    }
    // If we get here, the message was not a recognized ModuleInterface related message. Pass it to the user-defined callback.
    if (mis->custom_receive_function) mis->custom_receive_function(payload, length, packet_info, interface);
  }
}
