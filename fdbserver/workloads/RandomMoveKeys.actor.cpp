/*
 * RandomMoveKeys.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fdbrpc/simulator.h"
#include "fdbclient/StorageServerInterface.h"
#include "fdbclient/ManagementAPI.actor.h"
#include "fdbserver/MoveKeys.actor.h"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbserver/workloads/workloads.actor.h"
#include "fdbserver/ServerDBInfo.h"
#include "fdbserver/QuietDatabase.h"
#include "flow/actorcompiler.h"  // This must be the last #include.

struct MoveKeysWorkload : TestWorkload {
	bool enabled;
	double testDuration, meanDelay;
	double maxKeyspace;
	DatabaseConfiguration configuration;

	MoveKeysWorkload(WorkloadContext const& wcx)
		: TestWorkload(wcx)
	{
		enabled = !clientId && g_network->isSimulated(); // only do this on the "first" client
		meanDelay = getOption( options, LiteralStringRef("meanDelay"), 0.05 );
		testDuration = getOption( options, LiteralStringRef("testDuration"), 10.0 );
		maxKeyspace = getOption( options, LiteralStringRef("maxKeyspace"), 0.1 );
	}

	virtual std::string description() { return "MoveKeysWorkload"; }
	virtual Future<Void> setup( Database const& cx ) { return Void(); }
	virtual Future<Void> start( Database const& cx ) {
		return _start( cx, this );
	}

	ACTOR Future<Void> _start( Database cx, MoveKeysWorkload *self ) {
		if( self->enabled ) {
			// Get the database configuration so as to use proper team size
			state Transaction tr(cx);
			loop {
				try {
					Standalone<RangeResultRef> res = wait( tr.getRange(configKeys, 1000) );
					ASSERT( res.size() < 1000 );
					for( int i = 0; i < res.size(); i++ )
						self->configuration.set(res[i].key,res[i].value);
					break;
				} catch( Error &e ) {
					wait( tr.onError(e) );
				}
			}

			state int oldMode = wait( setDDMode( cx, 0 ) );
			TraceEvent("RMKStartModeSetting");
			wait( timeout( reportErrors( self->worker( cx, self ), "MoveKeysWorkloadWorkerError" ), self->testDuration, Void() ) );
			// Always set the DD mode back, even if we die with an error
			TraceEvent("RMKDoneMoving");
			wait(success( setDDMode( cx, oldMode ) ));
			TraceEvent("RMKDoneModeSetting");
		}
		return Void();
	}

	virtual double getCheckTimeout() { return testDuration/2 + 1; }
	virtual Future<bool> check( Database const& cx ) { return tag(delay(testDuration/2), true); }  // Give the database time to recover from our damage
	virtual void getMetrics( vector<PerfMetric>& m ) {
	}

	KeyRange getRandomKeys() const {
		double len = g_random->random01() * this->maxKeyspace;
		double pos = g_random->random01() * (1.0 - len);
		return KeyRangeRef( doubleToTestKey( pos ), doubleToTestKey( pos+len ) );
	}

	vector<StorageServerInterface> getRandomTeam(vector<StorageServerInterface> storageServers, int teamSize) {
		if( storageServers.size() < teamSize ) {
			TraceEvent(SevWarnAlways, "LessThanThreeStorageServers");
			throw operation_failed();
		}

		g_random->randomShuffle( storageServers );

		std::set<StorageServerInterface> t;
		std::set<Optional<Standalone<StringRef>>> machines;
		while (t.size() < teamSize && storageServers.size()) {
			auto s = storageServers.back();
			storageServers.pop_back();
			if( !machines.count( s.locality.zoneId() ) ) {
				machines.insert( s.locality.zoneId() );
				t.insert( s );
			}
		}

		if( t.size() < teamSize ) {
			TraceEvent(SevWarnAlways, "LessThanThreeUniqueMachines");
			throw operation_failed();
		}

		return vector<StorageServerInterface>(t.begin(), t.end());
	}

	ACTOR Future<Void> doMoveKeys(Database cx, MoveKeysWorkload *self, KeyRange keys, vector<StorageServerInterface> destinationTeam, MoveKeysLock lock ) {
		state TraceInterval relocateShardInterval("RelocateShard");
		state FlowLock fl1(1);
		state FlowLock fl2(1);
		std::string desc;
		for(int s=0; s<destinationTeam.size(); s++)
			desc += format("%s (%llx),", destinationTeam[s].address().toString().c_str(), destinationTeam[s].id().first());
		vector<UID> destinationTeamIDs;
		for(int s=0; s<destinationTeam.size(); s++)
			destinationTeamIDs.push_back( destinationTeam[s].id() );

		TraceEvent(relocateShardInterval.begin())
			.detail("KeyBegin", printable(keys.begin)).detail("KeyEnd", printable(keys.end))
			.detail("Priority", 0)
			.detail("Source", "RandomMoveKeys")
			.detail("DestinationTeam", desc);

		try {
			state Promise<Void> signal;
			wait( moveKeys( cx, keys, destinationTeamIDs, destinationTeamIDs, lock, signal, &fl1, &fl2, false, relocateShardInterval.pairID ) );
			TraceEvent(relocateShardInterval.end()).detail("Result","Success");
			return Void();
		} catch (Error& e) {
			TraceEvent(relocateShardInterval.end(), self->dbInfo->get().master.id()).error(e, true);
			throw;
		}
	}

	static void eliminateDuplicates( vector<StorageServerInterface>& servers ) {
		// The real data distribution algorithm doesn't want to deal with multiple servers
		// with the same address having keys.  So if there are two servers with the same address,
		// don't use either one (so we don't have to find out which of them, if any, already has keys).
		std::map<NetworkAddress, int> count;
		for(int s=0; s<servers.size(); s++)
			count[servers[s].address()]++;
		int o=0;
		for(int s=0; s<servers.size(); s++)
			if (count[servers[s].address()] == 1)
				servers[o++] = servers[s];
		servers.resize(o);
	}

	ACTOR Future<Void> forceMasterFailure( Database cx, MoveKeysWorkload *self ) {
		ASSERT( g_network->isSimulated() );
		loop {
			if( g_simulator.killZone( self->dbInfo->get().master.locality.zoneId(), ISimulator::Reboot, true ) )
				return Void();
			wait( delay(1.0) );
		}
	}

	ACTOR Future<Void> worker( Database cx, MoveKeysWorkload *self ) {
		state KeyRangeMap< vector<StorageServerInterface> > inFlight;
		state KeyRangeActorMap inFlightActors;
		state double lastTime = now();

		ASSERT( self->configuration.storageTeamSize > 0 );

		if(self->configuration.usableRegions > 1) { //FIXME: add support for generating random teams across DCs
			return Void();
		}

		loop {
			try {
				state MoveKeysLock lock = wait( takeMoveKeysLock(cx, UID()) );
				state vector<StorageServerInterface> storageServers = wait( getStorageServers( cx ) );
				eliminateDuplicates(storageServers);

				loop {
					wait( poisson( &lastTime, self->meanDelay ) );

					KeyRange keys = self->getRandomKeys();
					vector<StorageServerInterface> team = self->getRandomTeam(storageServers, self->configuration.storageTeamSize);

					// update both inFlightActors and inFlight key range maps, cancelling deleted RelocateShards
					vector<KeyRange> ranges;
					inFlightActors.getRangesAffectedByInsertion( keys, ranges );
					inFlightActors.cancel( KeyRangeRef( ranges.front().begin, ranges.back().end ) );
					inFlight.insert( keys, team );
					for(int r=0; r<ranges.size(); r++) {
						auto& rTeam = inFlight.rangeContaining(ranges[r].begin)->value();
						inFlightActors.insert( ranges[r], self->doMoveKeys( cx, self, ranges[r], rTeam, lock ) );
					}
				}
			} catch (Error& e) {
				if (e.code() != error_code_movekeys_conflict && e.code() != error_code_operation_failed )
					throw;
				wait( delay(FLOW_KNOBS->PREVENT_FAST_SPIN_DELAY) );
				// Keep trying to get the moveKeysLock
			}
		}
	}
};

WorkloadFactory<MoveKeysWorkload> MoveKeysWorkloadFactory("RandomMoveKeys");