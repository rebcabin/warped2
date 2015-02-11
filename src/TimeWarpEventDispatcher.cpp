#include "TimeWarpEventDispatcher.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <cmath>
#include <limits> // for std::numeric_limits<>::max();
#include <algorithm>    // for std::min
#include <chrono>   // for std::chrono::steady_clock
#include <cstring> // for std::memset
#include <iostream>
#include <cassert>

#include "Event.hpp"
#include "EventDispatcher.hpp"
#include "LTSFQueue.hpp"
#include "Partitioner.hpp"
#include "SimulationObject.hpp"
#include "TimeWarpMPICommunicationManager.hpp"
#include "TimeWarpMatternGVTManager.hpp"
#include "TimeWarpStateManager.hpp"
#include "TimeWarpOutputManager.hpp"
#include "TimeWarpEventSet.hpp"
#include "utility/memory.hpp"
#include "utility/warnings.hpp"

WARPED_REGISTER_POLYMORPHIC_SERIALIZABLE_CLASS(warped::EventMessage)
WARPED_REGISTER_POLYMORPHIC_SERIALIZABLE_CLASS(warped::Event)
WARPED_REGISTER_POLYMORPHIC_SERIALIZABLE_CLASS(warped::NegativeEvent)

namespace warped {

thread_local unsigned int TimeWarpEventDispatcher::thread_id;

TimeWarpEventDispatcher::TimeWarpEventDispatcher(unsigned int max_sim_time,
    unsigned int num_worker_threads,
    unsigned int num_schedulers,
    std::shared_ptr<TimeWarpCommunicationManager> comm_manager,
    std::unique_ptr<TimeWarpEventSet> event_set,
    std::unique_ptr<TimeWarpGVTManager> gvt_manager,
    std::unique_ptr<TimeWarpStateManager> state_manager,
    std::unique_ptr<TimeWarpOutputManager> output_manager,
    std::unique_ptr<TimeWarpFileStreamManager> twfs_manager) :
        EventDispatcher(max_sim_time), num_worker_threads_(num_worker_threads),
        num_schedulers_(num_schedulers), comm_manager_(comm_manager), 
        event_set_(std::move(event_set)), gvt_manager_(std::move(gvt_manager)), 
        state_manager_(std::move(state_manager)), 
        output_manager_(std::move(output_manager)), twfs_manager_(std::move(twfs_manager)) {}

void TimeWarpEventDispatcher::startSimulation(const std::vector<std::vector<SimulationObject*>>&
                                              objects) {
    initialize(objects);
    comm_manager_->waitForAllProcesses();

    // Create worker threads
    std::vector<std::thread> threads;
    for (unsigned int i = 0; i < num_worker_threads_; ++i) {
        auto thread(std::thread {&TimeWarpEventDispatcher::processEvents, this, i});
        thread.detach();
        threads.push_back(std::move(thread));
    }

    MessageFlags msg_flags = MessageFlags::None;

    auto gvt_start = std::chrono::steady_clock::now();
    bool calculate_gvt = false;

    // Flag that says we have started calculating minimum lvt of the objects on this node
    bool started_min_lvt = false;

    // Master thread main loop
    while (gvt_manager_->getGVT() < max_sim_time_) {
        // Get all received messages, handle messages, and get flags.
        msg_flags |= handleReceivedMessages();

        // Check if we have received new mattern token and we are not already in the middle
        //  of a local gvt calculation.
        checkLocalGVTStart(msg_flags, started_min_lvt);

        // Check if we have received a GVT update message
        checkGVTUpdate(msg_flags);

        // Check if calculate_gvt flag is set.
        // If not set, then check timer and set if timer period reached and a global gvt calculation
        //  is not already active.
        checkGlobalGVTStart(calculate_gvt, msg_flags, started_min_lvt, gvt_start);

        // Check if local gvt calculation complete.
        checkLocalGVTComplete(msg_flags, started_min_lvt);

        // Send all events in the remote event queue.
        sendRemoteEvents();
    }

    comm_manager_->finalize();
}

void TimeWarpEventDispatcher::processEvents(unsigned int id) {
    thread_id = id;

    while (gvt_manager_->getGVT() < max_sim_time_) {
        std::shared_ptr<Event> event = event_set_->getEvent(thread_id);
        if (event != nullptr) {

            unsigned int current_object_id = local_object_id_by_name_[event->receiverName()];
            SimulationObject* current_object = objects_by_name_[event->receiverName()];

            /*std::cout << "e = " << event << " , time = " << event->timestamp() 
                << " , receiver = " << event->receiverName() << " , sender = " 
                << event->sender_name_ << " , object time = " 
                << object_simulation_time_[current_object_id] << std::endl;*/

            // Check to see if object needs a rollback
            bool was_rolled_back = false;
            unsigned int null_thread = num_worker_threads_+1;
            while (!straggler_event_list_lock_[current_object_id].compare_exchange_weak(
                                                                            null_thread, id)) {
                null_thread = num_worker_threads_+1;
            }
            if (straggler_event_list_[current_object_id]) {
                /*std::cout << "roll - " << event << " , " << current_object_id << " , " 
                          << straggler_event_list_[current_object_id] << std::endl;*/
                rollback(straggler_event_list_[current_object_id], 
                                    current_object_id, current_object);
                if (straggler_event_list_[current_object_id]->event_type_ == 
                                                            EventType::NEGATIVE) {
                    event_set_->cancelEvent(current_object_id, 
                                            straggler_event_list_[current_object_id]);
                } else {
                    event_set_->startScheduling(current_object_id, 
                                            straggler_event_list_[current_object_id]);
                }
                straggler_event_list_[current_object_id] = 0;
                was_rolled_back = true;
            }
            straggler_event_list_lock_[current_object_id].store(num_worker_threads_+1);
            if (was_rolled_back) continue;

            // Handle negative event
            if (event->event_type_ == EventType::NEGATIVE) {
                event_set_->cancelEvent(current_object_id, event);
                continue;
            }

            if (min_lvt_flag_.load() > 0 && !calculated_min_flag_[thread_id]) {
                min_lvt_[thread_id] = std::min(send_min_[thread_id], event->timestamp());
                calculated_min_flag_[thread_id] = true;
                min_lvt_flag_.fetch_sub(1);
            }

            // Update simulation time
            object_simulation_time_[current_object_id] = event->timestamp();
            twfs_manager_->setObjectCurrentTime(event->timestamp(), current_object_id);

            // process event and get new events
            auto new_events = current_object->receiveEvent(*event);

            // Save state
            state_manager_->saveState(event, current_object_id, current_object);

            // Send new events
            for (auto& e: new_events) {
                if (e->event_type_ == EventType::POSITIVE) {
                    e->sender_name_ = current_object->name_;
                    e->counter_ = event_counter_by_obj_[current_object_id]++;
                    e->send_time_ = object_simulation_time_[current_object_id];
                }
                output_manager_->insertEvent(e, current_object_id);

                assert(e->timestamp() >= object_simulation_time_[current_object_id]);

                unsigned int node_id = object_node_id_by_name_[event->receiverName()];
                if (node_id == comm_manager_->getID()) {
                    // Local event
                    sendLocalEvent(e);
                } else {
                    // Remote event
                    enqueueRemoteEvent(e, node_id);
                }
            }

            // Move the next event from object into the schedule queue
            // Also transfer old event to processed queue
            event_set_->startScheduling(current_object_id, event);

        } else {
            // TODO, do something here
            //assert(false);
        }
    }
}

void TimeWarpEventDispatcher::insertIntoEventSet(
        unsigned int thread_index, unsigned int object_id, std::shared_ptr<Event> event) {

    if (event_set_->insertEvent(object_id, event)) return;

    // If the calling thread already has lock (during rollback), don't release
    bool calling_thread_has_lock = false;
    if (straggler_event_list_lock_[object_id].load() == thread_index) {
        calling_thread_has_lock = true;
    } else {
        unsigned int null_thread = num_worker_threads_+1;
        while (!straggler_event_list_lock_[object_id].compare_exchange_weak(
                                                            null_thread, thread_index)) {
            null_thread = num_worker_threads_+1;
        }
    }
    if (straggler_event_list_[object_id]) {
        if ((*event < *straggler_event_list_[object_id]) || 
            ((*event == *straggler_event_list_[object_id]) && 
             (event->event_type_ < straggler_event_list_[object_id]->event_type_))) {
            straggler_event_list_[object_id] = event;
            /*std::cout << "straggler1 - " << straggler_event_list_[object_id] 
                                            << " , " << object_id << std::endl;*/
        }
    } else {
        straggler_event_list_[object_id] = event;
        /*std::cout << "straggler2 - " << straggler_event_list_[object_id] 
                                            << " , " << object_id << std::endl;*/
    }
    if (!calling_thread_has_lock) {
        straggler_event_list_lock_[object_id].store(num_worker_threads_+1);
    }
}

MessageFlags TimeWarpEventDispatcher::receiveEventMessage(
        std::unique_ptr<TimeWarpKernelMessage> kmsg) {

    auto msg = unique_cast<TimeWarpKernelMessage, EventMessage>(std::move(kmsg));
    dynamic_cast<TimeWarpMatternGVTManager*>(gvt_manager_.get())->receiveEventUpdate(
        msg->gvt_mattern_color);

    unsigned int receiver_object_id = local_object_id_by_name_[msg->event->receiverName()];
    // Manager thread id equals worker thread count
    insertIntoEventSet(num_worker_threads_, receiver_object_id, msg->event);
    return MessageFlags::None;
}

void TimeWarpEventDispatcher::sendLocalEvent(std::shared_ptr<Event> event) {

    unsigned int receiver_object_id = local_object_id_by_name_[event->receiverName()];
    insertIntoEventSet(thread_id, receiver_object_id, event);

    if (min_lvt_flag_.load() > 0 && !calculated_min_flag_[thread_id]) {
        unsigned int send_min = send_min_[thread_id];
        send_min_[thread_id] = std::min(send_min, event->timestamp());
    }
}

void TimeWarpEventDispatcher::fossilCollect(unsigned int gvt) {
    twfs_manager_->fossilCollectAll(gvt);
    state_manager_->fossilCollectAll(gvt);
    output_manager_->fossilCollectAll(gvt);
    event_set_->fossilCollectAll(gvt);
}

void TimeWarpEventDispatcher::cancelEvents(
    std::unique_ptr<std::vector<std::shared_ptr<Event>>> events_to_cancel) {

    if (events_to_cancel->empty()) {
        return;
    }

    do {
        auto event = events_to_cancel->back();
        auto neg_event = std::make_shared<NegativeEvent>(event);
        events_to_cancel->pop_back();

        unsigned int receiver_node_id = object_node_id_by_name_[event->receiverName()];
        if (receiver_node_id != comm_manager_->getID()) {
            enqueueRemoteEvent(neg_event, receiver_node_id);
        } else {
            sendLocalEvent(neg_event);
        }
    } while (!events_to_cancel->empty());
}

void TimeWarpEventDispatcher::rollback(std::shared_ptr<Event> straggler_event, 
                            unsigned int local_object_id, SimulationObject* object) {

    unsigned int straggler_time = straggler_event->timestamp();
    twfs_manager_->rollback(straggler_time, local_object_id);

    //assert(straggler_time >= gvt_manager_->getGVT());

    std::shared_ptr<Event> restored_state_event =
        state_manager_->restoreState(straggler_event, local_object_id, object);
    auto events_to_cancel = output_manager_->rollback(straggler_event, local_object_id);
    assert(restored_state_event);
    assert((restored_state_event->timestamp() < straggler_event->timestamp())
           || (restored_state_event->timestamp() == 0));

    if (events_to_cancel != nullptr) {
        cancelEvents(std::move(events_to_cancel));
    }

    object_simulation_time_[local_object_id] = restored_state_event->timestamp();
    twfs_manager_->setObjectCurrentTime(restored_state_event->timestamp(), local_object_id);

    coastForward(object, straggler_event, restored_state_event);
}

void TimeWarpEventDispatcher::coastForward(SimulationObject* object, 
                std::shared_ptr<Event> straggler_event, std::shared_ptr<Event> restored_state_event) {

    unsigned int stop_time = straggler_event->timestamp();
    unsigned int current_object_id = local_object_id_by_name_[object->name_];

    auto events = event_set_->getEventsForCoastForward(
            current_object_id, straggler_event, restored_state_event);
    for (auto event_riterator = events->rbegin(); event_riterator != events->rend(); event_riterator++) {
        assert((*event_riterator)->timestamp() <= stop_time);
        assert((*event_riterator)->timestamp() >= object_simulation_time_[current_object_id]);
        assert((*event_riterator)->event_type_ != EventType::NEGATIVE);

        /*std::cout << "coast = " << *event_riterator << std::endl;*/

        object_simulation_time_[current_object_id] = (*event_riterator)->timestamp();
        twfs_manager_->setObjectCurrentTime((*event_riterator)->timestamp(), current_object_id);
        object->receiveEvent(**event_riterator);
        state_manager_->saveState(*event_riterator, current_object_id, object);
    }
}

void TimeWarpEventDispatcher::initialize(
        const std::vector<std::vector<SimulationObject*>>& objects) {
    unsigned int num_local_objects = objects[comm_manager_->getID()].size();
    event_set_->initialize(num_local_objects, num_schedulers_, num_worker_threads_);

    object_simulation_time_ = make_unique<unsigned int []>(num_local_objects);
    std::memset(object_simulation_time_.get(), 0, num_local_objects*sizeof(unsigned int));

    event_counter_by_obj_ = make_unique<std::atomic<unsigned long> []>(num_local_objects);

    // Create a structure to track which objects need to be rolled back
    straggler_event_list_ = make_unique<std::shared_ptr<Event> []>(num_local_objects);
    std::memset(straggler_event_list_.get(), 0, 
                    num_local_objects*sizeof(std::shared_ptr<Event>));
    straggler_event_list_lock_ = 
        make_unique<std::atomic<unsigned int> []>(num_local_objects);

    unsigned int partition_id = 0;
    for (auto& partition : objects) {
        unsigned int object_id = 0;
        for (auto& ob : partition) {
            if (partition_id == comm_manager_->getID()) {
                objects_by_name_[ob->name_] = ob;
                local_object_id_by_name_[ob->name_] = object_id;
                event_counter_by_obj_[object_id].store(0);
                straggler_event_list_lock_[object_id].store(num_worker_threads_+1);
                auto new_events = ob->createInitialEvents();
                for (auto& e: new_events) {
                    assert(e->event_type_ == EventType::POSITIVE);
                    e->sender_name_ = ob->name_;
                    e->counter_ = event_counter_by_obj_[object_id]++;
                    e->send_time_ = object_simulation_time_[object_id];

                    // Manager thread id equals worker thread count
                    insertIntoEventSet(num_worker_threads_, object_id, e);
                }
                object_id++;
            }
            object_node_id_by_name_[ob->name_] = partition_id;
        }
        partition_id++;
    }

    // Creates the state queues, output queues, and filestream queues for each local object
    state_manager_->initialize(num_local_objects);
    output_manager_->initialize(num_local_objects);
    twfs_manager_->initialize(num_local_objects);

    // Register message handlers
    gvt_manager_->initialize();

    std::function<MessageFlags(std::unique_ptr<TimeWarpKernelMessage>)> handler =
        std::bind(&TimeWarpEventDispatcher::receiveEventMessage, this,
        std::placeholders::_1);
    comm_manager_->addRecvMessageHandler(MessageType::EventMessage, handler);

    // Prepare local min lvt computation
    min_lvt_ = make_unique<unsigned int []>(num_worker_threads_);
    send_min_ = make_unique<unsigned int []>(num_worker_threads_);
    calculated_min_flag_ = make_unique<bool []>(num_worker_threads_);
    for (unsigned int i = 0; i < num_worker_threads_; i++) {
        min_lvt_[i] = std::numeric_limits<unsigned int>::max();
        send_min_[i] = std::numeric_limits<unsigned int>::max();
        calculated_min_flag_[i] = false;
    }
}

unsigned int TimeWarpEventDispatcher::getMinimumLVT() {
    unsigned int min = std::numeric_limits<unsigned int>::max();
    for (unsigned int i = 0; i < num_worker_threads_; i++) {
        min = std::min(min, min_lvt_[i]);
        // Reset send_min back to very large number for next calculation
        send_min_[i] = std::numeric_limits<unsigned int>::max();
        calculated_min_flag_[i] = false;
        min_lvt_[i] = std::numeric_limits<unsigned int>::max();
    }
    return min;
}

FileStream& TimeWarpEventDispatcher::getFileStream(
    SimulationObject *object, const std::string& filename, std::ios_base::openmode mode) {

    unsigned int local_object_id = local_object_id_by_name_[object->name_];
    TimeWarpFileStream* twfs = twfs_manager_->getFileStream(filename, mode, local_object_id);

    return *twfs;
}

void TimeWarpEventDispatcher::enqueueRemoteEvent(std::shared_ptr<Event> event,
    unsigned int receiver_id) {

    remote_event_queue_lock_.lock();
    remote_event_queue_.push_back(std::make_pair(event, receiver_id));
    remote_event_queue_lock_.unlock();
}

void TimeWarpEventDispatcher::sendRemoteEvents() {
    remote_event_queue_lock_.lock();
    while (!remote_event_queue_.empty()) {
        auto event = std::move(remote_event_queue_.front());
        remote_event_queue_.pop_front();

        MatternColor color = dynamic_cast<TimeWarpMatternGVTManager*>
            (gvt_manager_.get())->sendEventUpdate(event.first->timestamp());

        unsigned int receiver_id = event.second;
        auto event_msg = make_unique<EventMessage>(comm_manager_->getID(), receiver_id,
            event.first, color);

        comm_manager_->sendMessage(std::move(event_msg));
    }
    remote_event_queue_lock_.unlock();
}

MessageFlags TimeWarpEventDispatcher::handleReceivedMessages() {
    return comm_manager_->dispatchReceivedMessages();
}

void TimeWarpEventDispatcher::checkLocalGVTStart(MessageFlags &msg_flags, bool &started_min_lvt) {
    if (PENDING_MATTERN_TOKEN(msg_flags) && (min_lvt_flag_.load() == 0) && !started_min_lvt) {
        min_lvt_flag_.store(num_worker_threads_);
        started_min_lvt = true;
    }
}

void TimeWarpEventDispatcher::checkGVTUpdate(MessageFlags &msg_flags) {
    if (GVT_UPDATE(msg_flags)) {
        //fossilCollect(gvt_manager_->getGVT());
        msg_flags &= ~MessageFlags::GVTUpdate;
        dynamic_cast<TimeWarpMatternGVTManager*>(gvt_manager_.get())->reset();
    }
}

void TimeWarpEventDispatcher::checkGlobalGVTStart(bool &calculate_gvt, MessageFlags &msg_flags,
    bool &started_min_lvt, std::chrono::time_point<std::chrono::steady_clock> &gvt_start) {

    if (calculate_gvt && comm_manager_->getID() == 0) {
        // This will only return true if a token is not in circulation and we need to
        // start another one.
        MessageFlags flags = gvt_manager_->calculateGVT();
        msg_flags |= flags;
        checkLocalGVTStart(msg_flags, started_min_lvt);
        if (started_min_lvt) {
            calculate_gvt = false;
        }
    } else if (comm_manager_->getID() == 0) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>
            (now - gvt_start).count();
        if (elapsed >= gvt_manager_->getGVTPeriod()) {
            calculate_gvt = true;
            gvt_start = now;
        }
    }
}

void TimeWarpEventDispatcher::checkLocalGVTComplete(MessageFlags &msg_flags, bool &started_min_lvt) {
    if (PENDING_MATTERN_TOKEN(msg_flags) && (min_lvt_flag_.load() == 0) && started_min_lvt) {
        unsigned int local_min_lvt = getMinimumLVT();
        if (comm_manager_->getNumProcesses() > 1) {
            dynamic_cast<TimeWarpMatternGVTManager*>(gvt_manager_.get())->sendMatternGVTToken(
                local_min_lvt);
        } else {
            gvt_manager_->setGVT(local_min_lvt);
            std::cout << "GVT: " << local_min_lvt << std::endl;
            //fossilCollect(gvt_manager_->getGVT());
            dynamic_cast<TimeWarpMatternGVTManager*>(gvt_manager_.get())->reset();
        }
        msg_flags &= ~MessageFlags::PendingMatternToken;
        started_min_lvt = false;
    }
}

} // namespace warped


