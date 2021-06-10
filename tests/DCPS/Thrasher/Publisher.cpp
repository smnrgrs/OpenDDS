
#include "Publisher.h"
#include "ProgressIndicator.h"

#include <FooTypeTypeSupportImpl.h>
#include <tests/Utils/StatusMatching.h>

#include <dds/DdsDcpsInfrastructureC.h>
#include <dds/DCPS/Service_Participant.h>
#include <dds/DCPS/Marked_Default_Qos.h>
#include <dds/DCPS/PublisherImpl.h>
#include <dds/DCPS/WaitSet.h>
#include <dds/DCPS/RTPS/RtpsDiscovery.h>
#include <dds/DCPS/transport/framework/TransportRegistry.h>
#include <dds/DCPS/transport/framework/TransportConfig.h>
#include <dds/DCPS/transport/framework/TransportInst.h>

#include <string>

Publisher::Publisher(std::size_t samples_per_thread, bool durable)
  : samples_per_thread_(samples_per_thread)
  , durable_(durable)
  , thread_index_(0)
{
  ACE_DEBUG((LM_INFO, ACE_TEXT("(%P|%t) -> Publisher::Publisher\n")));
}

Publisher::~Publisher()
{
  ACE_DEBUG((LM_INFO, ACE_TEXT("(%P|%t) <- Publisher::~Publisher\n")));
}

int Publisher::svc()
{
  std::string pfx("(%P|%t) pub");
  try {
    DDS::DomainParticipantFactory_var dpf = TheParticipantFactory;
    DDS::DomainParticipant_var participant;

    int this_thread_index = 0;
    { // Scope for guard to serialize creating Entities.
      GuardType guard(lock_);
      this_thread_index = thread_index_++;
      char pub_i[8];
      ACE_OS::snprintf(pub_i, 8, "%d", this_thread_index);
      pfx += pub_i;
      ACE_DEBUG((LM_INFO, (pfx + "->started\n").c_str()));
      // Create Participant
      participant = dpf->create_participant(42, PARTICIPANT_QOS_DEFAULT, 0, OpenDDS::DCPS::DEFAULT_STATUS_MASK);

      // RTPS cannot be shared
      OpenDDS::DCPS::Discovery_rch disc = TheServiceParticipant->get_discovery(42);
      OpenDDS::RTPS::RtpsDiscovery_rch rd = OpenDDS::DCPS::dynamic_rchandle_cast<OpenDDS::RTPS::RtpsDiscovery>(disc);
      if (!rd.is_nil()) {
        char config_name[64], inst_name[64];
        ACE_TCHAR nak_depth[8];
        ACE_OS::snprintf(config_name, 64, "cfg_%d", this_thread_index);
        ACE_OS::snprintf(inst_name, 64, "rtps_%d", this_thread_index);
        // The 2 is a safety factor to allow for control messages.
        ACE_OS::snprintf(nak_depth, 8, ACE_TEXT("%lu"), 2 * samples_per_thread_);
        ACE_DEBUG((LM_INFO, (pfx + "->transport %C\n").c_str(), config_name));
        OpenDDS::DCPS::TransportConfig_rch config = TheTransportRegistry->create_config(config_name);
        OpenDDS::DCPS::TransportInst_rch inst = TheTransportRegistry->create_inst(inst_name, "rtps_udp");
        ACE_Configuration_Heap ach;
        ACE_Configuration_Section_Key sect_key;
        ach.open();
        ach.open_section(ach.root_section(), ACE_TEXT("not_root"), 1, sect_key);
        ach.set_string_value(sect_key, ACE_TEXT("use_multicast"), ACE_TEXT("0"));
        ach.set_string_value(sect_key, ACE_TEXT("nak_depth"), nak_depth);
        ach.set_string_value(sect_key, ACE_TEXT("heartbeat_period"), ACE_TEXT("200"));
        ach.set_string_value(sect_key, ACE_TEXT("heartbeat_response_delay"), ACE_TEXT("100"));
        inst->load(ach, sect_key);
        config->instances_.push_back(inst);
        TheTransportRegistry->bind_config(config_name, participant);
      }
    } // End of lock scope.

    if (!participant) {
      ACE_ERROR_RETURN((LM_ERROR, ACE_TEXT("%N:%l: create_participant failed!\n")), 1);
    }
    // Create Publisher
    DDS::Publisher_var publisher = participant->create_publisher(PUBLISHER_QOS_DEFAULT, 0, OpenDDS::DCPS::DEFAULT_STATUS_MASK);
    if (!publisher) {
      ACE_ERROR_RETURN((LM_ERROR, ACE_TEXT("%N:%l: create_publisher failed!\n")), 1);
    }
    // Register Type (FooType)
    FooTypeSupport_var ts = new FooTypeSupportImpl;
    if (ts->register_type(participant.in(), "") != DDS::RETCODE_OK) {
      ACE_ERROR_RETURN((LM_ERROR, ACE_TEXT("%N:%l: register_type failed!\n")), 1);
    }
    // Create Topic (FooTopic)
    DDS::Topic_var topic = participant->create_topic("FooTopic",
      CORBA::String_var(ts->get_type_name()), TOPIC_QOS_DEFAULT, 0, OpenDDS::DCPS::DEFAULT_STATUS_MASK);
    if (!topic) {
      ACE_ERROR_RETURN((LM_ERROR, ACE_TEXT("%N:%l: create_topic failed!\n")), 1);
    }
    // Create DataWriter
    DDS::DataWriterQos writer_qos;
    publisher->get_default_datawriter_qos(writer_qos);
    writer_qos.reliability.kind = DDS::RELIABLE_RELIABILITY_QOS;
    if (durable_) {
      writer_qos.durability.kind = DDS::TRANSIENT_LOCAL_DURABILITY_QOS;
    }
#ifndef OPENDDS_NO_OWNERSHIP_PROFILE
    writer_qos.history.depth = static_cast<CORBA::Long>(samples_per_thread_);
#endif
    DDS::DataWriter_var writer = publisher->create_datawriter(topic.in(), writer_qos, 0, OpenDDS::DCPS::DEFAULT_STATUS_MASK);
    if (!writer) {
      ACE_ERROR_RETURN((LM_ERROR, ACE_TEXT("%N:%l: create_datawriter failed!\n")), 1);
    }
    OpenDDS::DCPS::DataWriterImpl* wi = dynamic_cast<OpenDDS::DCPS::DataWriterImpl*>(writer.in());
    ACE_DEBUG((LM_INFO, (pfx + "  writer id: %C\n").c_str(), OpenDDS::DCPS::LogGuid(wi->get_repo_id()).c_str()));
    if (!durable_) {
      ACE_DEBUG((LM_INFO, (pfx + "->wait_match() early\n").c_str()));
      Utils::wait_match(writer, 1);
      ACE_DEBUG((LM_INFO, (pfx + "<-match found! early\n").c_str()));
    }
    FooDataWriter_var writer_i = FooDataWriter::_narrow(writer);
    if (!writer_i) {
      ACE_ERROR_RETURN((LM_ERROR, ACE_TEXT("%N:%l: FooDataWriter::_narrow failed!\n")), 1);
    }

    // The following is intentionally inefficient to stress various
    // pathways related to publication; we should be especially dull
    // and write only one sample at a time per writer.
    const std::string fmt(pfx + "  %d%% (%d samples sent)\n");
    ProgressIndicator progress(fmt.c_str(), samples_per_thread_);
    Foo foo;
    foo.key = 3;
    foo.x = (float) this_thread_index;
    DDS::InstanceHandle_t handle = writer_i->register_instance(foo);
    for (std::size_t i = 0; i < samples_per_thread_; ++i) {
      foo.y = (float) i;
      if (writer_i->write(foo, handle) != DDS::RETCODE_OK) {
        ACE_ERROR_RETURN((LM_ERROR, ACE_TEXT("%N:%l: svc() write failed!\n")), 1);
      }
      ++progress;
    }

    if (durable_) {
      ACE_DEBUG((LM_INFO, (pfx + "->wait_match()\n").c_str()));
      Utils::wait_match(writer, 1);
      ACE_DEBUG((LM_INFO, (pfx + "<-match found!\n").c_str()));
    }

    DDS::Duration_t interval = { 30, 0 };
    ACE_DEBUG((LM_INFO, (pfx + "  waiting for acks\n").c_str()));
    if (DDS::RETCODE_OK != writer->wait_for_acknowledgments(interval)) {
      ACE_ERROR_RETURN((LM_ERROR, (pfx + " ERROR: timed out waiting for acks!\n").c_str()), 1);
    }

    // Clean-up!
    ACE_DEBUG((LM_INFO, (pfx + "<-delete_contained_entities\n").c_str()));
    participant->delete_contained_entities();
    ACE_DEBUG((LM_INFO, (pfx + "<-delete_participant\n").c_str()));
    dpf->delete_participant(participant.in());
  } catch (const CORBA::Exception& e) {
    e._tao_print_exception("caught in svc()");
    return 1;
  }
  return 0;
}
