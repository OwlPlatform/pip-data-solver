/*
 * Copyright (c) 2013 Bernhard Firner
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 * or visit http://www.gnu.org/licenses/gpl-2.0.html
 */

/*******************************************************************************
 * @file pip-data-solver.cpp
 * Offer the data from pipsqueak sensors to the world model.
 * Raw packets are taken from the aggregator, their headers are interpreted,
 * and their data is offered to the world model as on-demand data.
 *
 * @author Bernhard Firner
 ******************************************************************************/

#include <string>
#include <thread>
#include <vector>
#include <queue>
#include <mutex>

#include <signal.h>

//TODO Remove this (used for usleep) and replace with C++0x sleep mechanism
#include <unistd.h>

#include <owl/solver_aggregator_connection.hpp>
#include <owl/solver_world_connection.hpp>
#include <owl/netbuffer.hpp>
#include <owl/sample_data.hpp>
#include <owl/world_model_protocol.hpp>

using std::u16string;
using std::vector;
using std::queue;

using world_model::grail_time;


//Global variable for the signal handler.
bool interrupted = false;
//Signal handler.
void handler(int signal) {
  psignal( signal, "Received signal ");
  if (interrupted) {
    std::cerr<<"Aborting.\n";
    // This is the second time we've received the interrupt, so just exit.
    exit(-1);
  }
  std::cerr<<"Shutting down...\n";
  interrupted = true;
}

///Convert a transmitter to an ID of the form <physical layer>.<transmitter id>
std::u16string txerToUString(unsigned int phy, TransmitterID& txid) {
	std::string str = std::to_string(phy) + "." + std::to_string(txid.lower);
	return u16string(str.begin(), str.end());
}

int main(int ac, char** av) {
	if (ac == 2 and std::string(av[1]) == "-?") {
		std::cout<< "name: Pipsqueak Data Solver\n";
		std::cout<< "arguments: aggregator agg_solver worldmodel wm_solver\n";
		std::cout<< "description: Interprets pipsqueak data as transient types.\n";
		std::cout<< "provides: temperature\n";
		std::cout<< "provides: binary state\n";
		return 0;
	}

	if (4 > ac) {
		std::cerr<<"This program needs at least 4 arguments:\n";
		std::cerr<<"\t"<<av[0]<<" [<aggregator ip> <aggregator port>]+ <world model ip> <world model port>\n";
		std::cerr<<"Any number of aggregator ip/port pairs may be provided to connect to multiple servers.\n";
		return 0;
	}

	//Grab the ip and ports for the servers and aggregator
	std::vector<SolverAggregator::NetTarget> servers;
	for (int s_num = 1; s_num < ac - 2; s_num += 2) {
		std::string server_ip(av[s_num]);
		uint16_t server_port = std::stoi(std::string((av[s_num + 1])));
		servers.push_back(SolverAggregator::NetTarget{server_ip, server_port});
	}
	//World model IP and port
	std::string wm_ip(av[ac-2]);
	int wm_port = std::stoi(std::string((av[ac-1])));


	//Set up the solver world model connection;
	std::string origin = "pip-data-solver";
	//Provide variance as a transient type
	//Link variance is between a transmitter and a receiver.
	//Average variance is the average of all link variances for a transmitter
	std::vector<std::pair<u16string, bool>> type_pairs{{u"temperature", true},
		{u"binary state", true}};
	SolverWorldModel swm(wm_ip, wm_port, type_pairs, u16string(origin.begin(), origin.end()));
	if (not swm.connected()) {
		std::cerr<<"Could not connect to the world model - aborting.\n";
		return 0;
	}

	//A single producer, single consumer queue for incoming samples
	//Need to provide a maximum size for the ringbuffer used in the
	//queue, using 1000 (hopefully the aggregator does not get ahead
	//of this solver).
	queue<SampleData> incoming_samples;
	std::mutex sample_mutex;

	//Get all data from all physical layers (0 represents any physical layer)
	aggregator_solver::Rule pip_rule;
	pip_rule.physical_layer = 1;
	//Request data as it arrives
	pip_rule.update_interval = 0;
	//Callback pushes the new sample into the sample queue
	auto packet_callback = [&](SampleData& s) {
		std::unique_lock<std::mutex> lck(sample_mutex);
		incoming_samples.push(s);
	};
	SolverAggregator aggregator(servers, packet_callback);
	aggregator.updateRules(aggregator_solver::Subscription{pip_rule});

	auto pop = [&](SampleData& data) {
		std::unique_lock<std::mutex> lck(sample_mutex);
		if (incoming_samples.empty()) {
			return false;
		}
		else {
			data = incoming_samples.front();
			incoming_samples.pop();
			return true;
		}
	};

	//Keep processing samples until the program gets an interrupt signal
	while (not interrupted) {
		SampleData next;
		//If there is a next sample collect it
		if (pop(next)) {
			//TODO
			//Check to see if a group of samples was generated by a single
			//transmitter at multiple receivers

			//If there is any data then interpret it
			if (0 < next.sense_data.size()) {
				unsigned char header = next.sense_data[0];
				//There is only one header type right now, a 7bit temperature + 1
				//binary bit.
				//TODO Make this more generic, have these defined in
				//the sample_data header.
				const uint8_t decode = 0x80;
				const uint8_t temp_binary = 0x01;
				const uint8_t unknown = 0x7E;
				
				vector<SolverWorldModel::AttrUpdate> solns;
				auto tx_name = txerToUString(next.physical_layer, next.tx_id);
				if (not (header & decode)) {
					size_t index = 1;
					if (header & unknown) {
						std::cerr<<"Header data from "<<std::string(tx_name.begin(), tx_name.end())<<" is unknown.\n";
					}
					if (header & temp_binary and
							index < next.sense_data.size()) {
						SolverWorldModel::AttrUpdate temp_soln{u"temperature", world_model::getGRAILTime(), tx_name, vector<uint8_t>()};
						double temp = (next.sense_data.at(index) >> 1) - 40.0;
						std::cerr<<"Temperature from "<<std::string(tx_name.begin(), tx_name.end())<<" is "<<temp<<'\n';
						pushBackVal(temp, temp_soln.data);

						SolverWorldModel::AttrUpdate bin_soln{u"binary state", world_model::getGRAILTime(), tx_name, vector<uint8_t>()};
						uint8_t bin = next.sense_data.at(index) & 0x01;
						pushBackVal(bin, bin_soln.data);

						solns.push_back(temp_soln);
						solns.push_back(bin_soln);
						//Next data is at the next index.
						++index;
					}
				}
				//Do not create URIs for these entries, just send the data
				if (not solns.empty()) {
					//std::cerr<<"Updating "<<solns.size()<<" attributes.\n";
					swm.sendData(solns, false);
				}
			}
		}
	}

	return 0;
}

