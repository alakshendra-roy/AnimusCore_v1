#pragma once
// Phase 9: Distributed Cloud Orchestration & Clustering.
//
// Windows-only: layered directly on animus_transport.hpp's Schannel mTLS
// SecureChannel, per the explicit Phase 9 scoping decision -- real gRPC +
// Protobuf would be this project's first external build dependency, so
// inter-node RPC here is a small hand-rolled binary protocol (fixed-size
// structs + a length-prefixed message envelope, see
// SecureChannel::send_message/recv_message) reusing the same mTLS
// transport, RBAC-cert machinery, and "zero external dependency" property
// established in Phase 8, rather than a parallel wire stack.
//
// What's replicated: control-plane commands (currently just AddRule) that
// every node's local animus::Engine must apply identically, not telemetry
// itself -- each node keeps ingesting/processing its own telemetry locally
// on the existing zero-copy hot path (animus.hpp); only the *rule set* that
// hot path evaluates against goes through consensus, so cluster membership
// changes cost nothing on the ingestion fast path.
//
// This is deliberately a "lite" Raft (see the whitepaper term used in the
// Phase 9 request): in-memory-only log/term state (no durable storage, so
// a node that restarts loses its Raft state -- acceptable for a demo,
// would need a WAL for production), no log compaction/snapshotting, and
// blocking sockets with no per-RPC timeout (a cleanly-stopped peer fails
// fast because its listening socket closes; a hung-but-open peer would
// stall a round rather than failing over promptly -- a real deployment
// would want send/recv timeouts here). The core algorithm itself --
// randomized election timeouts, term-based safety, log-consistency-checked
// replication with conflict truncation, and the "only commit entries from
// the leader's own current term" safety rule -- is real Raft, not a stub.
#if !defined(_WIN32)
#error "animus_cluster.hpp is Windows-only (built on animus_transport.hpp's Schannel transport)."
#endif

#include "animus_transport.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace animus {
namespace cluster {

    using NodeId = uint32_t;

    // The one command type this "lite" cluster replicates today: register a
    // detection rule on every node's local engine. Fixed-size POD so log
    // entries need no variable-length field serialization beyond the
    // entry-count prefix in AppendEntriesArgs below.
#pragma pack(push, 1)
    struct AddRuleCommand {
        uint32_t rule_id;
        uint32_t event_id;
        uint64_t threshold;
        uint8_t comparator;
        uint32_t severity;
    };

    // is_noop marks a leader-election no-op entry (see start_election()'s
    // success branch) rather than a real AddRuleCommand -- apply_committed_
    // entries_locked() skips calling engine_.add_rule() for these. Needed
    // because of a genuine Raft subtlety (the paper's Section 5.4.2): a
    // leader can never advance commit_index_ by counting replicas of an
    // entry from a PREVIOUS term, only its own -- so a brand-new leader
    // that inherits an already-majority-replicated-but-not-yet-committed
    // entry from the previous leader's term can get permanently stuck
    // unable to commit it if nothing new is ever proposed through the new
    // leader. Appending one no-op entry in the new leader's own term as
    // the first thing it does, then replicating it, guarantees there is
    // always a current-term entry whose commitment (once it reaches a
    // majority) carries every earlier, still-pending entry forward with
    // it -- this was found and fixed after reproducing exactly this
    // stall in a real 3-node failover run (10 runs; ~20% hit it).
    struct LogEntry {
        uint64_t term;
        uint8_t is_noop;
        AddRuleCommand command;
    };
#pragma pack(pop)

    enum class Role : uint8_t { Follower = 0, Candidate = 1, Leader = 2 };

    inline const char* role_name(Role r) noexcept {
        switch (r) {
        case Role::Follower: return "Follower";
        case Role::Candidate: return "Candidate";
        case Role::Leader: return "Leader";
        }
        return "?";
    }

    enum class ProposeResult : uint8_t { Ok = 0, NotLeader = 1, Failed = 2 };

    // Static cluster membership: every node knows every peer's id, address,
    // and expected certificate CN up front (no dynamic membership changes
    // in this "lite" implementation).
    struct PeerConfig {
        NodeId id;
        std::string host;
        uint16_t port;
        std::wstring cert_cn;
    };

    namespace wire {
        enum class MsgType : uint8_t {
            RequestVoteReq = 1,
            RequestVoteResp = 2,
            AppendEntriesReq = 3,
            AppendEntriesResp = 4,
        };

#pragma pack(push, 1)
        struct RequestVoteArgs {
            uint64_t term;
            uint32_t candidate_id;
            uint64_t last_log_index;
            uint64_t last_log_term;
        };
        struct RequestVoteResult {
            uint64_t term;
            uint8_t vote_granted;
        };
        struct AppendEntriesHeader {
            uint64_t term;
            uint32_t leader_id;
            uint64_t prev_log_index;
            uint64_t prev_log_term;
            uint64_t leader_commit;
            uint32_t entry_count;
        };
        struct AppendEntriesResult {
            uint64_t term;
            uint8_t success;
            uint64_t match_index;
        };
#pragma pack(pop)

        template <typename T>
        std::vector<uint8_t> pack(MsgType type, const T& body) {
            std::vector<uint8_t> out(1 + sizeof(T));
            out[0] = static_cast<uint8_t>(type);
            std::memcpy(out.data() + 1, &body, sizeof(T));
            return out;
        }

        inline std::vector<uint8_t> pack_append_entries(const AppendEntriesHeader& hdr, const std::vector<LogEntry>& entries) {
            std::vector<uint8_t> out(1 + sizeof(AppendEntriesHeader) + entries.size() * sizeof(LogEntry));
            out[0] = static_cast<uint8_t>(MsgType::AppendEntriesReq);
            std::memcpy(out.data() + 1, &hdr, sizeof(AppendEntriesHeader));
            if (!entries.empty()) {
                std::memcpy(out.data() + 1 + sizeof(AppendEntriesHeader), entries.data(), entries.size() * sizeof(LogEntry));
            }
            return out;
        }
    } // namespace wire

    // One persistent, mTLS-authenticated, direction-specific link to a
    // single peer: this node dials the peer and is the ONLY side that
    // issues RPC requests over the resulting connection (the peer's
    // symmetric outbound connection back to this node carries requests the
    // other way). This avoids needing to multiplex concurrent requests
    // from both ends over one socket.
    class PeerLink {
    public:
        std::mutex mtx;
        std::unique_ptr<transport::SecureChannel> channel;
    };

    class RaftNode {
    public:
        RaftNode(NodeId id, std::vector<PeerConfig> peers, transport::CertContextPtr my_cert,
            transport::TrustedRoot& trust, animus::Engine& engine, uint16_t listen_port)
            : id_(id), peers_(std::move(peers)), my_cert_(std::move(my_cert)), trust_(trust),
            engine_(engine), listen_port_(listen_port), rng_(std::random_device{}()) {
            for (const auto& p : peers_) cn_to_node_id_[p.cert_cn] = p.id;
        }

        ~RaftNode() { stop(); }

        RaftNode(const RaftNode&) = delete;
        RaftNode& operator=(const RaftNode&) = delete;

        void start() {
            if (running_.exchange(true)) return;
            reset_election_deadline();
            listener_thread_ = std::thread([this] { listener_loop(); });
            tick_thread_ = std::thread([this] { tick_loop(); });
        }

        // Closes the listener and every peer connection, then joins all
        // background threads. Used both for orderly shutdown and to
        // simulate a node failure in the failover demo -- a peer trying to
        // reach a stopped node's closed listener socket fails fast
        // (connection refused) rather than hanging.
        void stop() {
            if (!running_.exchange(false)) return;
            {
                std::lock_guard<std::mutex> lock(listener_mutex_);
                listener_socket_.close();
            }
            {
                std::lock_guard<std::mutex> lock(links_mutex_);
                for (auto& kv : links_) {
                    std::lock_guard<std::mutex> lk(kv.second->mtx);
                    kv.second->channel.reset();
                }
            }
            {
                // Force-close every still-open INBOUND connection too --
                // closing only the listener stops new peers from
                // connecting, but an already-accepted handler thread is
                // blocked in recv_message() on its own socket, which the
                // listener close does nothing to. See the comment at
                // inbound_channels_'s use-site in listener_loop().
                std::lock_guard<std::mutex> lock(handlers_mutex_);
                for (auto& ch : inbound_channels_) ch->force_close();
            }
            if (tick_thread_.joinable()) tick_thread_.join();
            if (listener_thread_.joinable()) listener_thread_.join();
            std::lock_guard<std::mutex> lock(handlers_mutex_);
            for (auto& t : handler_threads_) if (t.joinable()) t.join();
            handler_threads_.clear();
            inbound_channels_.clear();
        }

        Role role() const noexcept { return role_.load(); }
        bool is_leader() const noexcept { return role_.load() == Role::Leader; }
        uint64_t current_term() const {
            std::lock_guard<std::mutex> lock(state_mutex_);
            return current_term_;
        }
        NodeId leader_hint() const {
            std::lock_guard<std::mutex> lock(state_mutex_);
            return has_leader_ ? current_leader_ : 0;
        }
        size_t log_size() const {
            std::lock_guard<std::mutex> lock(state_mutex_);
            return log_.size();
        }
        uint64_t committed_count() const {
            std::lock_guard<std::mutex> lock(state_mutex_);
            return commit_index_;
        }

        // Leader-only: appends the command to the local log, replicates it
        // to a majority, and blocks (bounded by timeout_ms) until it is
        // committed and applied to this node's own engine. Followers
        // reject immediately with NotLeader + a hint at who the leader is
        // (if known), mirroring how a real Raft-backed client library
        // redirects a write to the current leader instead of retrying
        // blindly.
        ProposeResult propose(const AddRuleCommand& cmd, NodeId* leader_hint_out, int timeout_ms = 3000) {
            size_t my_index;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (role_.load() != Role::Leader) {
                    if (leader_hint_out) *leader_hint_out = has_leader_ ? current_leader_ : 0;
                    return ProposeResult::NotLeader;
                }
                log_.push_back(LogEntry{ current_term_, /*is_noop=*/0, cmd });
                my_index = log_.size();
            }
            replicate_to_all_peers();

            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
            while (std::chrono::steady_clock::now() < deadline) {
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    if (commit_index_ >= my_index) return ProposeResult::Ok;
                    if (role_.load() != Role::Leader) {
                        if (leader_hint_out) *leader_hint_out = has_leader_ ? current_leader_ : 0;
                        return ProposeResult::NotLeader;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return ProposeResult::Failed;
        }

    private:
        // ------------------------------------------------------------
        // Networking: listener + per-connection server handler + a
        // lazily-(re)connected outbound PeerLink per peer.
        // ------------------------------------------------------------
        void listener_loop() {
            transport::TcpSocket sock;
            try {
                sock = transport::TcpSocket::listen_on(listen_port_);
            }
            catch (const std::exception&) {
                running_.store(false);
                return;
            }
            {
                std::lock_guard<std::mutex> lock(listener_mutex_);
                listener_socket_ = std::move(sock);
            }
            while (running_.load()) {
                transport::TcpSocket accepted;
                try {
                    accepted = listener_socket_.accept_one();
                }
                catch (const std::exception&) {
                    break; // listener socket closed by stop()
                }
                // The channel is heap-allocated and registered in
                // inbound_channels_ (rather than owned locally inside the
                // handler thread) so that stop() -- running on a different
                // thread -- can reach in and force_close() it: a handler
                // thread blocked in recv_message() on a peer that has
                // nothing more to send has no other way to learn the node
                // is shutting down, and stop()'s handler_threads_ join
                // would otherwise hang forever waiting for it.
                auto channel = std::make_shared<transport::SecureChannel>(std::move(accepted));
                {
                    std::lock_guard<std::mutex> lock(handlers_mutex_);
                    inbound_channels_.push_back(channel);
                    handler_threads_.emplace_back([this, channel] { handle_inbound(*channel); });
                }
            }
        }

        void handle_inbound(transport::SecureChannel& channel) {
            if (!channel.handshake_as_server(my_cert_.get())) return;

            std::string err;
            if (!transport::verify_certificate_chain(channel.peer_certificate(), trust_, nullptr, &err)) return;

            std::wstring peer_cn = transport::get_subject_common_name(channel.peer_certificate());
            auto it = cn_to_node_id_.find(peer_cn);
            if (it == cn_to_node_id_.end()) return; // unrecognized peer identity, refuse
            NodeId peer_id = it->second;

            for (;;) {
                std::vector<uint8_t> req;
                if (!channel.recv_message(req)) return;
                if (req.empty()) return;
                std::vector<uint8_t> resp = dispatch_request(peer_id, req);
                if (resp.empty() || !channel.send_message(resp)) return;
            }
        }

        std::vector<uint8_t> dispatch_request(NodeId from, const std::vector<uint8_t>& req) {
            auto type = static_cast<wire::MsgType>(req[0]);
            if (type == wire::MsgType::RequestVoteReq && req.size() == 1 + sizeof(wire::RequestVoteArgs)) {
                wire::RequestVoteArgs args;
                std::memcpy(&args, req.data() + 1, sizeof(args));
                wire::RequestVoteResult result = handle_request_vote(from, args);
                return wire::pack(wire::MsgType::RequestVoteResp, result);
            }
            if (type == wire::MsgType::AppendEntriesReq && req.size() >= 1 + sizeof(wire::AppendEntriesHeader)) {
                wire::AppendEntriesHeader hdr;
                std::memcpy(&hdr, req.data() + 1, sizeof(hdr));
                // Validate the claimed entry count against the message we
                // actually received BEFORE sizing a vector off it -- a
                // corrupt or hostile entry_count must not drive an
                // unbounded allocation.
                size_t expected = 1 + sizeof(hdr) + static_cast<size_t>(hdr.entry_count) * sizeof(LogEntry);
                if (req.size() != expected) return {};
                std::vector<LogEntry> entries(hdr.entry_count);
                if (hdr.entry_count > 0) {
                    std::memcpy(entries.data(), req.data() + 1 + sizeof(hdr), entries.size() * sizeof(LogEntry));
                }
                wire::AppendEntriesResult result = handle_append_entries(from, hdr, entries);
                return wire::pack(wire::MsgType::AppendEntriesResp, result);
            }
            return {};
        }

        PeerLink& link_for(NodeId peer_id) {
            std::lock_guard<std::mutex> lock(links_mutex_);
            auto it = links_.find(peer_id);
            if (it != links_.end()) return *it->second;
            auto link = std::make_unique<PeerLink>();
            PeerLink& ref = *link;
            links_.emplace(peer_id, std::move(link));
            return ref;
        }

        const PeerConfig* peer_config(NodeId peer_id) const {
            for (const auto& p : peers_) if (p.id == peer_id) return &p;
            return nullptr;
        }

        // Sends [1 request byte + payload] to peer_id over its dedicated
        // outbound link, (re)connecting and mTLS-handshaking first if
        // necessary, and verifying the peer's certificate resolves to the
        // exact node id we intended to call -- not merely "some trusted
        // node" -- before trusting the response.
        bool call_rpc(NodeId peer_id, const std::vector<uint8_t>& request, std::vector<uint8_t>& response) {
            const PeerConfig* cfg = peer_config(peer_id);
            if (!cfg) return false;
            PeerLink& link = link_for(peer_id);
            std::lock_guard<std::mutex> lock(link.mtx);

            if (!link.channel) {
                try {
                    // A short connect timeout matters much more here than
                    // in Phase 8's client/server demo: a dead cluster peer
                    // is a NORMAL, expected condition every election round
                    // has to route around (see connect_to()'s comment in
                    // animus_transport.hpp for why the OS's default
                    // timeout is unsuitable), and it must fail well within
                    // a single election round rather than stall the whole
                    // round's worker-thread join.
                    transport::TcpSocket sock = transport::TcpSocket::connect_to(cfg->host, cfg->port, /*timeout_ms=*/200);
                    auto ch = std::make_unique<transport::SecureChannel>(std::move(sock));
                    if (!ch->handshake_as_client(my_cert_.get(), L"localhost")) return false;
                    std::string err;
                    if (!transport::verify_certificate_chain(ch->peer_certificate(), trust_, nullptr, &err)) return false;
                    if (transport::get_subject_common_name(ch->peer_certificate()) != cfg->cert_cn) return false;
                    link.channel = std::move(ch);
                }
                catch (const std::exception&) {
                    return false;
                }
            }

            if (!link.channel->send_message(request) || !link.channel->recv_message(response)) {
                link.channel.reset(); // drop a broken link; next call reconnects
                return false;
            }
            return true;
        }

        // ------------------------------------------------------------
        // Raft algorithm. All access to term/log/commit state goes
        // through state_mutex_; network I/O in call_rpc() above always
        // happens with state_mutex_ NOT held, so a slow/unreachable peer
        // blocks only the thread calling it, never the whole node.
        // ------------------------------------------------------------
        uint64_t last_log_term_locked() const {
            return log_.empty() ? 0 : log_.back().term;
        }

        wire::RequestVoteResult handle_request_vote(NodeId candidate_id, const wire::RequestVoteArgs& args) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (args.term > current_term_) {
                current_term_ = args.term;
                role_.store(Role::Follower);
                has_voted_this_term_ = false;
            }
            bool vote_granted = false;
            if (args.term >= current_term_) {
                bool log_ok = (args.last_log_term > last_log_term_locked()) ||
                    (args.last_log_term == last_log_term_locked() && args.last_log_index >= log_.size());
                if ((!has_voted_this_term_ || voted_for_ == candidate_id) && log_ok) {
                    vote_granted = true;
                    has_voted_this_term_ = true;
                    voted_for_ = candidate_id;
                    reset_election_deadline_locked();
                }
            }
            return wire::RequestVoteResult{ current_term_, vote_granted ? uint8_t(1) : uint8_t(0) };
        }

        wire::AppendEntriesResult handle_append_entries(NodeId leader_id, const wire::AppendEntriesHeader& hdr, const std::vector<LogEntry>& entries) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (hdr.term < current_term_) {
                return wire::AppendEntriesResult{ current_term_, 0, 0 };
            }
            // Only clear the "already voted this term" flag when the term
            // actually advances -- NOT on every heartbeat from the term's
            // legitimate leader. Otherwise a stale concurrent candidate
            // still canvassing for the SAME term could win a second vote
            // from a follower who already voted, after that follower's
            // vote flag was wrongly cleared by an unrelated heartbeat --
            // a real violation of Raft's one-vote-per-term Election Safety
            // property, not merely a cosmetic bug.
            if (hdr.term > current_term_) {
                current_term_ = hdr.term;
                has_voted_this_term_ = false;
            }
            role_.store(Role::Follower);
            current_leader_ = leader_id;
            has_leader_ = true;
            reset_election_deadline_locked();

            if (hdr.prev_log_index > 0) {
                if (log_.size() < hdr.prev_log_index) {
                    return wire::AppendEntriesResult{ current_term_, 0, 0 };
                }
                if (log_[static_cast<size_t>(hdr.prev_log_index) - 1].term != hdr.prev_log_term) {
                    log_.resize(hdr.prev_log_index - 1);
                    return wire::AppendEntriesResult{ current_term_, 0, 0 };
                }
            }

            size_t idx = static_cast<size_t>(hdr.prev_log_index);
            for (const LogEntry& e : entries) {
                if (idx < log_.size()) {
                    if (log_[idx].term != e.term) {
                        log_.resize(idx);
                        log_.push_back(e);
                    }
                }
                else {
                    log_.push_back(e);
                }
                ++idx;
            }

            if (hdr.leader_commit > commit_index_) {
                commit_index_ = std::min<uint64_t>(hdr.leader_commit, log_.size());
                apply_committed_entries_locked();
            }
            return wire::AppendEntriesResult{ current_term_, 1, log_.size() };
        }

        void apply_committed_entries_locked() {
            while (last_applied_ < commit_index_) {
                const LogEntry& entry = log_[static_cast<size_t>(last_applied_)];
                if (!entry.is_noop) {
                    const AddRuleCommand& cmd = entry.command;
                    engine_.add_rule(cmd.rule_id, cmd.event_id, cmd.threshold, cmd.comparator, cmd.severity);
                }
                ++last_applied_;
            }
        }

        void reset_election_deadline_locked() {
            std::uniform_int_distribution<int> dist(kElectionTimeoutMinMs, kElectionTimeoutMaxMs);
            election_deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(dist(rng_));
        }
        void reset_election_deadline() {
            std::lock_guard<std::mutex> lock(state_mutex_);
            reset_election_deadline_locked();
        }

        void tick_loop() {
            while (running_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs));
                if (!running_.load()) break;
                if (role_.load() == Role::Leader) {
                    replicate_to_all_peers();
                }
                else {
                    bool expired;
                    {
                        std::lock_guard<std::mutex> lock(state_mutex_);
                        expired = std::chrono::steady_clock::now() >= election_deadline_;
                    }
                    if (expired) start_election();
                }
            }
        }

        void start_election() {
            uint64_t term_at_election;
            uint64_t last_idx, last_term;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                ++current_term_;
                role_.store(Role::Candidate);
                voted_for_ = id_;
                has_voted_this_term_ = true;
                has_leader_ = false;
                reset_election_deadline_locked();
                term_at_election = current_term_;
                last_idx = log_.size();
                last_term = last_log_term_locked();
            }

            wire::RequestVoteArgs args{ term_at_election, id_, last_idx, last_term };
            std::vector<uint8_t> req = wire::pack(wire::MsgType::RequestVoteReq, args);

            std::atomic<int> votes{ 1 }; // vote for self
            std::vector<std::thread> workers;
            for (const auto& p : peers_) {
                // Capture peer_id BY VALUE, not a reference to the loop
                // variable p -- p is rebound/destroyed on each iteration
                // while these threads are still running (they're only
                // joined after this loop finishes below), so a captured
                // reference to p would dangle.
                NodeId peer_id = p.id;
                workers.emplace_back([this, peer_id, &req, &votes, term_at_election] {
                    std::vector<uint8_t> resp;
                    if (!call_rpc(peer_id, req, resp)) return;
                    if (resp.size() != 1 + sizeof(wire::RequestVoteResult)) return;
                    wire::RequestVoteResult result;
                    std::memcpy(&result, resp.data() + 1, sizeof(result));
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    if (result.term > current_term_) {
                        current_term_ = result.term;
                        role_.store(Role::Follower);
                        has_voted_this_term_ = false;
                        reset_election_deadline_locked(); // re-randomize rather than keep whatever was set when OUR candidacy began, for the same dueling-candidate mitigation as the wider timeout spread above
                        return;
                    }
                    if (result.vote_granted && result.term == term_at_election) votes.fetch_add(1);
                    });
            }
            for (auto& t : workers) t.join();

            bool became_leader;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                size_t majority = (peers_.size() + 1) / 2 + 1;
                became_leader = role_.load() == Role::Candidate && current_term_ == term_at_election &&
                    static_cast<size_t>(votes.load()) >= majority;
                if (became_leader) {
                    role_.store(Role::Leader);
                    current_leader_ = id_;
                    has_leader_ = true;
                    for (const auto& p : peers_) {
                        next_index_[p.id] = log_.size() + 1;
                        match_index_[p.id] = 0;
                    }
                    // See LogEntry::is_noop's comment: without a current-
                    // term entry to anchor commit advancement to, an
                    // already-majority-replicated-but-not-yet-committed
                    // entry inherited from the PREVIOUS leader's term can
                    // get stuck uncommitted forever if nothing new is ever
                    // proposed afterward. Appending and immediately
                    // replicating this guarantees that never happens.
                    log_.push_back(LogEntry{ current_term_, /*is_noop=*/1, AddRuleCommand{} });
                }
            }
            // replicate_to_all_peers() acquires state_mutex_ itself, so
            // this must run after the lock above is released.
            if (became_leader) replicate_to_all_peers();
        }

        void replicate_to_all_peers() {
            uint64_t term_snapshot, commit_snapshot;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (role_.load() != Role::Leader) return;
                term_snapshot = current_term_;
                commit_snapshot = commit_index_;
            }

            std::vector<std::thread> workers;
            for (const auto& p : peers_) {
                // Same by-value capture rationale as start_election() above.
                NodeId peer_id = p.id;
                workers.emplace_back([this, peer_id, term_snapshot, commit_snapshot] {
                    replicate_to_peer(peer_id, term_snapshot, commit_snapshot);
                    });
            }
            for (auto& t : workers) t.join();

            // Recompute commit_index_: highest index replicated to a
            // majority (this node + peers with match_index_ >= N) whose
            // entry's term equals the leader's CURRENT term -- Raft never
            // commits a prior-term entry purely by count, only by being
            // carried forward alongside a current-term entry, to avoid the
            // classic "leader crashes right after replicating, a new
            // leader overwrites it" safety hole.
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (role_.load() != Role::Leader || current_term_ != term_snapshot) return;
            for (uint64_t n = log_.size(); n > commit_index_; --n) {
                if (log_[static_cast<size_t>(n) - 1].term != current_term_) continue;
                size_t count = 1; // self
                for (const auto& p : peers_) if (match_index_[p.id] >= n) ++count;
                if (count >= (peers_.size() + 1) / 2 + 1) {
                    commit_index_ = n;
                    apply_committed_entries_locked();
                    break;
                }
            }
        }

        void replicate_to_peer(NodeId peer_id, uint64_t term_snapshot, uint64_t commit_snapshot) {
            uint64_t next_idx;
            std::vector<LogEntry> entries;
            uint64_t prev_log_index, prev_log_term;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (role_.load() != Role::Leader || current_term_ != term_snapshot) return;
                next_idx = next_index_[peer_id];
                prev_log_index = next_idx > 0 ? next_idx - 1 : 0;
                prev_log_term = (prev_log_index > 0 && prev_log_index <= log_.size())
                    ? log_[static_cast<size_t>(prev_log_index) - 1].term : 0;
                for (uint64_t i = next_idx; i <= log_.size() && entries.size() < kMaxEntriesPerRpc; ++i) {
                    entries.push_back(log_[static_cast<size_t>(i) - 1]);
                }
            }

            wire::AppendEntriesHeader hdr{ term_snapshot, id_, prev_log_index, prev_log_term,
                commit_snapshot, static_cast<uint32_t>(entries.size()) };
            std::vector<uint8_t> req = wire::pack_append_entries(hdr, entries);

            std::vector<uint8_t> resp;
            if (!call_rpc(peer_id, req, resp)) return;
            if (resp.size() != 1 + sizeof(wire::AppendEntriesResult)) return;
            wire::AppendEntriesResult result;
            std::memcpy(&result, resp.data() + 1, sizeof(result));

            std::lock_guard<std::mutex> lock(state_mutex_);
            if (result.term > current_term_) {
                current_term_ = result.term;
                role_.store(Role::Follower);
                has_voted_this_term_ = false;
                return;
            }
            if (role_.load() != Role::Leader || current_term_ != term_snapshot) return;
            if (result.success) {
                next_index_[peer_id] = prev_log_index + entries.size() + 1;
                match_index_[peer_id] = prev_log_index + entries.size();
            }
            else {
                uint64_t& ni = next_index_[peer_id];
                if (ni > 1) --ni;
            }
        }

        static constexpr int kTickMs = 30;
        // A wider spread lowers the odds that two simultaneously-viable
        // candidates (e.g. the two survivors in a 3-node cluster after the
        // third dies) keep re-colliding round after round -- collision
        // probability per round is roughly proportional to (RPC round-trip
        // time) / (spread), so widening the spread instead of just hoping
        // for a lucky draw is the standard Raft mitigation.
        static constexpr int kElectionTimeoutMinMs = 300;
        static constexpr int kElectionTimeoutMaxMs = 900;
        static constexpr size_t kMaxEntriesPerRpc = 64;

        NodeId id_;
        std::vector<PeerConfig> peers_;
        transport::CertContextPtr my_cert_;
        transport::TrustedRoot& trust_;
        animus::Engine& engine_;
        uint16_t listen_port_;
        std::unordered_map<std::wstring, NodeId> cn_to_node_id_;

        std::atomic<bool> running_{ false };
        std::atomic<Role> role_{ Role::Follower };
        std::mt19937 rng_;

        mutable std::mutex state_mutex_;
        uint64_t current_term_ = 0;
        NodeId voted_for_ = 0;
        bool has_voted_this_term_ = false;
        std::vector<LogEntry> log_;
        uint64_t commit_index_ = 0;
        uint64_t last_applied_ = 0;
        NodeId current_leader_ = 0;
        bool has_leader_ = false;
        std::chrono::steady_clock::time_point election_deadline_;
        std::unordered_map<NodeId, uint64_t> next_index_;
        std::unordered_map<NodeId, uint64_t> match_index_;

        std::mutex listener_mutex_;
        transport::TcpSocket listener_socket_;
        std::thread listener_thread_;
        std::thread tick_thread_;
        std::mutex handlers_mutex_;
        std::vector<std::thread> handler_threads_;
        std::vector<std::shared_ptr<transport::SecureChannel>> inbound_channels_;
        std::mutex links_mutex_;
        std::unordered_map<NodeId, std::unique_ptr<PeerLink>> links_;
    };

} // namespace cluster
} // namespace animus
