/*
 * Copyright (c) 2015, University Corporation for Atmospheric Research
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without 
 * modification, are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice, 
 * this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 
 * 3. Neither the name of the copyright holder nor the names of its contributors
 * may be used to endorse or promote products derived from this software without
 * specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" 
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR 
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF 
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
 * WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <cstdio>
#include <cstdlib>
#include <vector>
#include <map>
#include <set>
#include <iostream>
#include <fstream>
#include <cstring>
#include <cstring>
#include <stdint.h>
#include <sys/types.h>
#include <sys/dir.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sstream>
#include <signal.h>
#include <syslog.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <errno.h>
#include <exception>

#include <libcgroup.h>

#include <cgroup_context.h>

#include <log.h>
#include <proc_utils.h>


void enumerate_tasks(char* cgpath, uid_t victim, std::vector<pid_t>& cached_task_list);

extern "C"
{
char is_oom(struct cgroup_context* cgc)
{
	char* path;
	asprintf(&path, "/%s/%s/memory.oom_control", cgc->cgroup_path, cgc->cgroup_name);
	std::ifstream oc(path, std::ifstream::in);
	char isoom = -1;
	while(oc.good())
	{
		std::string token;
		oc >> token;
		if(token == "under_oom")
		{
			oc >> isoom;
		}
	}
	oc.close();
	free(path);
	if(isoom == -1) abort();
	if(isoom) return(1);
	return(0);	
}
}

void get_cgroup_from_pid(pid_t pid, std::string& result)
{
	char* path;
	asprintf(&path, "/proc/%d/cgroup", pid);
	std::ifstream gcl(path, std::ifstream::in);
	try {
		while(gcl.good())
		{
			std::string token;
			gcl >> token;
			size_t delim = 0;
			delim = token.find_first_of(':');
			token.erase(0, delim);
			if(token.find("memory:/") != std::string::npos)
			{
				delim = token.find_first_of(':');
				if((delim+1) > token.length())
				{
					result.append("/;");
				}
				else
				{
					result.append(token.substr(delim+1));
					result.append(";");
				}
			}
		}
	gcl.close();
	}
	catch (std::exception& e)
	{
		//This shouldn't really ever happen
		//see gcc bug 53984
		slog(LOG_ALERT, "Unexpected Exception reading cgroup file\n");
		result.append(e.what());
	}
	free(path);
}

// Relies on procfs, but Linux.
int get_pid_state(pid_t pid, char* state)
{
        FILE* pid_fh;
        char* procfs_pid_stat;
        pid_t ppid;

        // File location on procfs
        asprintf(&procfs_pid_stat,"/proc/%d/stat",pid)

        // open, read state, close --> get 3rd field and 4th, but probably won't need it ever.
        pid_fh = fopen(procfs_pid_stat,'r');
        fscanf(pid_fh, "%*s %*s %c %d",state,&ppid);
        fclose(pid_fh);

        return 0; // in case we need return error or something else later.

}

void sigkill_victim(pid_t pid)
{
	uid_t victim_uid;
	std::string cgroups;
	char* log_msg;
	victim_uid = get_uid(pid);
	get_cgroup_from_pid(pid, cgroups);
	asprintf(&log_msg, "killing UID:%u PID %d; cgroups: %s\n", victim_uid, pid, 
			 cgroups.c_str()
			);
	slog(LOG_ALERT, log_msg);
	free(log_msg);

	if (pid_state == 'D' || pid_state == 'Z') return;
	kill(pid, SIGKILL);

}
void kill_victim(struct cgroup_context* cgc, uid_t victim_uid)
{

	std::vector<pid_t> cached_task_list;
	//get PID list
	char* cgpath;
	asprintf(&cgpath, "/%s/%s/", cgc->cgroup_path, cgc->cgroup_name);	
	enumerate_tasks(cgpath, victim_uid, cached_task_list);
	free(cgpath);

	struct rlimit core_limit;
	core_limit.rlim_cur = 0;
	core_limit.rlim_max = 0;

	//Freeze all of user's processes
	for(std::vector<pid_t>::iterator i = cached_task_list.begin();
		i!= cached_task_list.end();
		i++)
	{
		cgroup_attach_task_pid(cgc->purgatory, *i);
	}

	char* root_freezer_path;
	char* root_memory_path;
	asprintf(&root_freezer_path, "/%s/tasks", cgc->freezer_path);
	asprintf(&root_memory_path, "/%s/tasks", cgc->cgroup_path);

	for(std::vector<pid_t>::iterator i = cached_task_list.begin();
		i!= cached_task_list.end();
		i++)
	{
		FILE* root_freezer = fopen(root_freezer_path,"w");
		FILE* root_memory = fopen(root_memory_path, "w");
		sigkill_victim(*i);
		fprintf(root_memory, "%d", *i);
		fclose(root_memory);
		fprintf(root_freezer, "%d", *i);
		fclose(root_freezer);
	}	

	free(root_memory_path);
	free(root_freezer_path);
}

void enumerate_tasks(char* cgpath, uid_t victim_uid, std::vector<pid_t>& cached_task_list)
{	
	pid_t pid;
	char* task_path;
	asprintf(&task_path, "/%s/tasks", cgpath);
	std::ifstream task_list(task_path,std::ifstream::in);
	while(task_list.good())
	{
		task_list >> pid;
		if(!(task_list.good())) //last read seems to be garbage
		{
			break;
		}
		uid_t uid = get_uid(pid);
		if(uid == victim_uid)
		{
			cached_task_list.push_back(pid);
		}
	}
	task_list.close();
	free(task_path);

	DIR* cgd = opendir(cgpath);
	if(cgd == NULL) slog(LOG_ALERT, "Error opening cgroup directory: %s\n", cgpath);
	struct dirent* de;
	struct stat stat_buf;
	int r;
	while((de=readdir(cgd))!=NULL)
	{
		char* tmp_path;
		asprintf(&tmp_path, "%s/%s/", cgpath, de->d_name);
		r = stat(tmp_path, &stat_buf);
		if(r!=0)
		{
			if(errno != ENOTDIR)
			{
		 	  slog(LOG_ALERT,
			   "enumerate_tasks(): stat() error code: %s on \"%s\"",
		  	   strerror(errno), tmp_path);
			}
		}
		else
		{
			if(S_ISDIR(stat_buf.st_mode) && de->d_name[0]!='.')
			{
				enumerate_tasks(tmp_path, victim_uid, cached_task_list);
			}
		}
		free(tmp_path);
	}
	closedir(cgd);
}

void enumerate_users(char* cgpath, std::map<uid_t, memory_t>& user_list)
{
	char* task_path;

	asprintf(&task_path, "/%s/tasks", cgpath);
	std::ifstream task_list(task_path,std::ifstream::in);
	pid_t pid;
	while(task_list.good())
	{
		task_list >> pid;
		if(!(task_list.good())) //last read seems to be garbage
		{
			break;
		}
		uid_t uid = get_uid(pid);
		memory_t rss = get_rss(pid);
		if(user_list.find(uid)==user_list.end())
		{
			user_list[uid] = rss;
		}
		else
		{
			user_list[uid] = user_list[uid] + rss;
		}
	}
	task_list.close();
	free(task_path);

	DIR* cgd = opendir(cgpath);
	if(cgd == NULL) slog(LOG_ALERT, "Error opening cgroup directory: %s\n", cgpath);
	struct dirent* de;
	struct stat stat_buf;
	int r;
	while((de=readdir(cgd))!=NULL)
	{
		char* tmp_path;
		asprintf(&tmp_path, "%s/%s/", cgpath, de->d_name);
		r = stat(tmp_path, &stat_buf);
		if(r!=0) {
			if(errno != ENOTDIR)
			{
			  slog(LOG_ALERT, 
			  "enumerate_users(): stat() error code: %s on \"%s\"",
			  strerror(errno), tmp_path);
			}
		}
		else
		{
			if(S_ISDIR(stat_buf.st_mode) && de->d_name[0]!='.')
			{
				enumerate_users(tmp_path, user_list);
			}
		}
		free(tmp_path);
	}
	closedir(cgd);
}

extern "C"
{
	int find_victim(struct cgroup_context* cgc)
{
	std::map<uid_t,memory_t> user_list;
	char* cgpath;
	asprintf(&cgpath, "/%s/%s/", cgc->cgroup_path, cgc->cgroup_name);
	enumerate_users(cgpath, user_list);	
	free(cgpath);
	
	if(user_list.size() < 1)
	{
		return(-1);
	}

	memory_t max = (user_list.begin())->second;
	uid_t max_uid = (user_list.begin())->first;
	for(std::map<uid_t,memory_t>::iterator i = user_list.begin();
		i!=user_list.end();
		i++)
	{
		if(i->second > max) 
			{
				max_uid = i->first;
				max = i->second;
			}
	}
	kill_victim(cgc, max_uid);
	return(0);
}
		
}
