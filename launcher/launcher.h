/* launcher.h                                                    -*- C++ -*-
   Eric Robert, 28 February 2013
   Copyright (c) 2012 Datacratic.  All rights reserved.
   
   Common launcher task structures
*/

#pragma once

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include "jml/arch/exception.h"
#include "jml/arch/timers.h"
#include "jml/utils/ring_buffer.h"
#include "soa/jsoncpp/json.h"
#include "soa/service/service_base.h"
#include "soa/service/message_loop.h"
#include "soa/service/typed_message_channel.h"

namespace Datacratic {

struct Launcher
{
    struct Task
    {
        Task() : pid(-1), log(false), delay(5.0) {
        }

        std::string const & getName() const {
            return name;
        }

        void restart() {
            stop();
            start();
        }

        void start() {
            spawn();
            ML::sleep(delay);

            for(auto & item : children) {
                item.start();
            }
        }

        void stop() {
            if(pid != -1 && kill(pid, 0) != -1) {
                int res = kill(pid, SIGTERM);
                if(res == -1) {
                    throw ML::Exception(errno, "cannot kill process");
                }

                int status = 0;
                res = waitpid(pid, &status, 0);
                if(res == -1) {
                    throw ML::Exception(errno, "failed to wait for process to shutdown");
                }

                std::cout << "killed " << name << std::endl;
            }

            pid = -1;

            for(auto & item : children) {
                item.stop();
            }
        }

        Task * findTask(int pid) const {
            if(this->pid == pid) {
                return (Task *) this;
            }

            for(auto & item : children) {
                Task * task = item.findTask(pid);
                if(task) {
                    return task;
                }
            }

            return 0;
        }

        void script(int & i, std::ostream & file) {
            file << "tmux new-window -d -t rtb:" << ++i << " -n '" << name << "' 'tail -F ./logs/" << name << ".log'" << std::endl;
            for(auto & item : children) {
                item.script(i, file);
            }
        }

        void script(int & i, std::ostream & file, std::string const & node) {
            file << "tmux new-window -d -t rtb:" << ++i << " -n '" << name << "' 'ssh " << node << " \"tail -F " << root << "/logs/" << name << ".log\"'" << std::endl;
            for(auto & item : children) {
                item.script(i, file, node);
            }
        }

        friend std::ostream & operator<<(std::ostream & stream, Task & task) {
            task.print(stream);
            return stream;
        }

        static Task createFromJson(Json::Value const & json) {
            Task result;
            for(auto i = json.begin(), end = json.end(); i != end; ++i) {
                if(i.memberName() == "children") {
                    auto & json = *i;
                    if(!json.empty() && !json.isArray()) {
                        throw ML::Exception("children is not an array");
                    }

                    for(auto j = json.begin(), end = json.end(); j != end; ++j) {
                        auto & json = *j;
                        result.children.push_back(createFromJson(json));
                    }
                }
                else if(i.memberName() == "name") {
                    result.name = i->asString();
                }
                else if(i.memberName() == "path") {
                    result.path = i->asString();
                }
                else if(i.memberName() == "root") {
                    result.root = i->asString();
                }
                else if(i.memberName() == "log") {
                    result.log = i->asBool();
                }
                else if(i.memberName() == "delay") {
                    result.delay = i->asDouble();
                }
                else if(i.memberName() == "arg") {
                    auto & json = *i;
                    if(!json.empty() && !json.isArray()) {
                        throw ML::Exception("'arg' is not an array");
                    }

                    for(auto j = json.begin(), end = json.end(); j != end; ++j) {
                        auto & json = *j;
                        result.arg.push_back(json.asString());
                    }
                }
                else {
                    throw ML::Exception("unknown task field '" + i.memberName() + "'");
                }
            }

            return result;
        }

    private:
        void print(std::ostream & stream, std::string indent = "") {
            stream << indent << name << " (" << pid << ")" << std::endl;
            stream << indent << "$> " << path;

            for(auto & item : arg) {
                stream << " " << item;
            }

            stream << std::endl;
            indent += "  ";

            for(auto & item : children) {
                item.print(stream, indent);
            }
        }

        std::vector<char const *> makeArgs() {
            std::vector<char const *> result;

            result.push_back(path.c_str());
            for(auto & item : arg) {
                result.push_back(item.c_str());
            }

            result.push_back(0);
            return result;
        }

        std::vector<char const *> makeEnvs() {
            std::vector<char const *> result;
            result.push_back(0);
            return result;
        }

        void spawn() {
            std::cout << "launch " << name << std::endl;
            pid = fork();

            if(pid == -1) {
                throw ML::Exception(errno, "fork failed");
            }

            if(pid == 0) {
                signal(SIGTERM, SIG_DFL);
                signal(SIGKILL, SIG_DFL);

                int res = prctl(PR_SET_PDEATHSIG, SIGHUP);
                if(res == -1) {
                    throw ML::Exception(errno, "prctl failed");
                }

                if(log) {
                    redirect();
                }

                res = chdir(root.c_str());
                if(res == -1) {
                    throw ML::Exception(errno, "chdir failed");
                }

                std::vector<char const *> args = makeArgs();
                std::vector<char const *> envs = makeEnvs();

                res = execvpe(path.c_str(), (char **) &args[0], (char **) &envs[0]);
                if (res == -1) {
                    throw ML::Exception(errno, "process failed to start");
                }

                throw ML::Exception(errno, "execvp failed");
            }
        }

        void redirect() {
            std::string filename = ML::format("./logs/%s-%d.log", name, getpid());

            int fd = open(filename.c_str(), O_WRONLY|O_CREAT, 0666);
            if(fd == -1) {
                throw ML::Exception(errno, "open log '" + name + "' failed");
            }

            if(-1 == dup2(fd, 1)) {
                throw ML::Exception(errno, "failed to redirect STDOUT to file");
            }

            if(-1 == dup2(1, 2)) {
                throw ML::Exception(errno, "failed to redirect STDERR to STDOUT");
            }

            std::string ln = ML::format("ln -s -f ./%s-%d.log ./logs/%s.log", name, getpid(), name);
            if(-1 == system(ln.c_str())) {
                throw ML::Exception(errno, "failed to create symbolic link");
            }

            close(fd);
        }

        int pid;
        std::vector<Task> children;
        std::string name;
        std::string path;
        std::string root;
        std::vector<std::string> arg;
        bool log;
        double delay;
    };

    struct Node
    {
        std::string const & getName() const {
            return name;
        }

        Task * findTask(int pid) const {
            for(auto & item : tasks) {
                Task * task = item.findTask(pid);
                if(task) {
                    return task;
                }
            }

            return 0;
        }

        void restart() {
            for(auto & item : tasks) {
                item.restart();
            }
        }

        void script(int & i, std::ostream & file) {
            for(auto & item : tasks) {
                item.script(i, file);
            }
        }

        void script(int & i, std::ostream & file, std::string const & node) {
            for(auto & item : tasks) {
                item.script(i, file, node);
            }
        }

        friend std::ostream & operator<<(std::ostream & stream, Node & node) {
            stream << node.name << std::endl;
            for(int i = 0; i != node.tasks.size(); ++i) {
                stream << "task #" << i << std::endl << node.tasks[i] << std::endl;
            }

            return stream;
        }

        static Node createFromJson(Json::Value const & json) {
            Node result;
            for(auto i = json.begin(), end = json.end(); i != end; ++i) {
                if(i.memberName() == "tasks") {
                    auto & json = *i;
                    if(!json.empty() && !json.isArray()) {
                        throw ML::Exception("'tasks' is not an array");
                    }

                    for(auto j = json.begin(), end = json.end(); j != end; ++j) {
                        auto & json = *j;
                        result.tasks.push_back(Task::createFromJson(json));
                    }
                }
                else if(i.memberName() == "name") {
                    result.name = i->asString();
                }
                else if(i.memberName() == "root") {
                    result.root = i->asString();
                }
                else {
                    throw ML::Exception("unknown node field '" + i.memberName() + "'");
                }
            }

            return result;
        }

    private:
        std::string name;
        std::string root;
        std::vector<Task> tasks;
    };

    struct Sequence
    {
        Node * getNode(std::string const & name) {
            for(auto & item : nodes) {
                if(item.getName() == name) {
                    return &item;
                }
            }

            return 0;
        }

        void script(std::string const & filename, std::string const & sh, std::string const & node, bool master) {
            std::ofstream file(sh);
            if(!file) {
                throw ML::Exception("cannot create " + sh + " script");
            }

            file << "#!/bin/bash" << std::endl;
            file << std::endl;
            file << "tmux kill-session -t rtb" << std::endl;
            file << "tmux new-session -d -s rtb './build/x86_64/bin/launcher --node " << node << " --script " << sh << (master ? " --master" : "") << " --launch" << " " << filename << "'" << std::endl;
            file << "tmux rename-window 'launcher'" << std::endl;

            int i = 0;
            for(int j = 0; j != nodes.size(); ++j) {
                auto & item = nodes[j];
                auto & name = item.getName();
                if(name == node) {
                    item.script(i, file);
                }
                else if(master) {
                    item.script(i, file, name);
                }
            }

            file << "tmux attach -t rtb" << std::endl;
            file.close();

            chmod(sh.c_str(), 0755);
        }

        friend std::ostream & operator<<(std::ostream & stream, Sequence & sequence) {
            for(auto & item : sequence.nodes) {
                stream << "node:" << std::endl << item << std::endl;
            }

            return stream;
        }

        static Sequence createFromJson(Json::Value const & json) {
            Sequence result;
            for(auto i = json.begin(), end = json.end(); i != end; ++i) {
                if(i.memberName() == "nodes") {
                    auto & json = *i;
                    if(!json.empty() && !json.isArray()) {
                        throw ML::Exception("'nodes' is not an array");
                    }

                    for(auto j = json.begin(), end = json.end(); j != end; ++j) {
                        auto & json = *j;
                        result.nodes.push_back(Node::createFromJson(json));
                    }
                }
                else {
                    throw ML::Exception("unknown launch sequence field '" + i.memberName() + "'");
                }
            }

            return result;
        }

    private:
        std::vector<Node> nodes;
    };

    struct Service : public MessageLoop
    {
        void run(Json::Value const & root, std::string const & name, std::string const & filename, std::string const & sh, bool launch, bool master) {
            sequence = Datacratic::Launcher::Sequence::createFromJson(root);

            if(!sh.empty()) {
                sequence.script(filename, sh, name, master);
            }

            if(launch) {
                node = sequence.getNode(name);
                if(!node) {
                    throw ML::Exception("cannot find node " + name);
                }

                int res = system("mkdir -p ./logs");
                if(res == -1) {
                    throw ML::Exception("cannot create ./logs directory");
                }

                start();

                struct sigaction sa;
                memset(&sa, 0, sizeof(sa));
                sa.sa_handler = &Service::sigchld;
                sigaction(SIGCHLD, &sa, 0);

                node->restart();

                for(;;) {
                    ML::sleep(1.0);
                }
            }
        }

        static Service & get() {
            static Service instance;
            return instance;
        }

    private:
        Service() : events(65536) {
            events.onEvent = std::bind<void>(&Service::onDeath, this, std::placeholders::_1);
            addSource("Launcher::Service::events", events);
        }

        void onDeath(int pid) {
            Task * item = node->findTask(pid);

            std::time_t now = std::time(0);
            std::cerr << "crash! " << (item ? item->getName() : "?") << " detected at " << std::asctime(std::localtime(&now)) << std::endl;
            if(item) {
                item->restart();
            }
        }

        static void sigchld(int pid) {
            for(;;) {
                int status = 0;
                int pid = waitpid(-1, &status, WNOHANG);
                if(pid == 0 || pid == -1) {
                    break;
                }

                Service::get().events.push(pid);
            }
        }

        // child death events
        TypedMessageSink<int> events;

        // node associated with this service
        Node * node;

        // launching sequence
        Sequence sequence;
    };
};

} // namespace RTBKIT
