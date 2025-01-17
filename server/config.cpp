/***
    This file is part of snapcast
    Copyright (C) 2014-2018  Johannes Pohl

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
***/

#include "config.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <fstream>
#include <cerrno>
#include "common/snapException.h"
#include "common/strCompat.h"
#include "common/utils/file_utils.h"
#include "aixlog.hpp"

using namespace std;


Config::Config()
{
}


Config::~Config()
{
	save();
}


void Config::init(const std::string& root_directory, const std::string& user, const std::string& group)
{
	string dir;
	if (!root_directory.empty())
		dir = root_directory;
	else if (getenv("HOME") == NULL)
		dir = "/var/lib/snapserver/";
	else
		dir = getenv("HOME");

	if (!dir.empty() && (dir.back() != '/'))
		dir += "/";

	if (dir.find("/var/lib/snapserver") == string::npos)
		dir += ".config/snapserver/";

	int status = utils::file::mkdirRecursive(dir.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
	if ((status != 0) && (errno != EEXIST))
		throw SnapException("failed to create settings directory: \"" + dir + "\": " + cpt::to_string(errno));

	filename_ = dir + "server.json";
	SLOG(NOTICE) << "Settings file: \"" << filename_ << "\"\n";

	int fd;
	if ((fd = open(filename_.c_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) == -1)
	{
		if (errno == EACCES)
			throw std::runtime_error("failed to open file \"" + filename_ + "\", permission denied (error " + cpt::to_string(errno) + ")");
		else 
			throw std::runtime_error("failed to open file \"" + filename_ + "\", error " + cpt::to_string(errno));
	}
	close(fd);

	if (!user.empty() && !group.empty())
	{
		try
		{
			utils::file::do_chown(dir, user, group);
			utils::file::do_chown(filename_, user, group);
		}
		catch(const std::exception& e)
		{
			SLOG(ERROR) << "Exception in chown: " << e.what() << "\n";
		}
	}

	try
	{
		ifstream ifs(filename_, std::ifstream::in);
		if (ifs.good())
		{
			json j;
			ifs >> j;
			if (j.count("ConfigVersion"))
			{
				json jGroups = j["Groups"];
				for (auto it = jGroups.begin(); it != jGroups.end(); ++it)
				{
					GroupPtr group = make_shared<Group>(*it);
					//group->fromJson(*it);
//					if (client->id.empty() || getClientInfo(client->id))
//						continue;
					groups.push_back(group);
				}
			}
		}
	}
	catch(const std::exception& e)
	{
		LOG(ERROR) << "Error reading config: " << e.what() << "\n";
	}
	
	// Check if we have a 'default' group, create it if not
	GroupPtr ptr = getDefaultGroup();
}


void Config::save()
{
	if (filename_.empty())
		init();
	std::ofstream ofs(filename_.c_str(), std::ofstream::out|std::ofstream::trunc);
	json clients = {
		{"ConfigVersion", 2},
		{"Groups", getGroups()}
	};
	ofs << std::setw(4) << clients;
	ofs.close();
}


ClientInfoPtr Config::getClientInfo(const std::string& clientId) const
{
	if (clientId.empty())
		return nullptr;

	for (auto group: groups)
	{
		for (auto client: group->clients)
		{
			if (client->id == clientId)
				return client;
		}
	}

	return nullptr;
}

GroupPtr Config::addClientInfo(ClientInfoPtr client)
{
	GroupPtr group = getGroupFromClient(client);
	if (!group)
	{
		group = getDefaultGroup();
		group->addClient(client);
	}
	return group;
}


GroupPtr Config::addClientInfo(const std::string& clientId)
{
	ClientInfoPtr client = getClientInfo(clientId);
	if (!client)
		client = make_shared<ClientInfo>(clientId);
	return addClientInfo(client);
}

GroupPtr Config::getDefaultGroup()
{
	if (groups.size() == 0)
	{
		SLOG(INFO) << "No groups in config, adding '" << DEFAULT_GROUP << "'!\n";
		addGroup(DEFAULT_GROUP);
	}
	return groups[0];
}

GroupPtr Config::addGroup(const std::string& name)
{
	GroupPtr group = make_shared<Group>(name);
	groups.push_back(group);
	return group;
}



GroupPtr Config::getGroup(const std::string& groupId) const
{
	for (auto group: groups)
	{
		if (group->id == groupId)
			return group;
	}

	return nullptr;
}

GroupPtr Config::getGroupFromName(const std::string& name) const
{
	for (auto group: groups)
	{
		if (group->name == name)
			return group;
	}

	return nullptr;
}


GroupPtr Config::getGroupFromClient(const std::string& clientId)
{
	for (auto group: groups)
	{
		for (auto c: group->clients)
		{
			if (c->id == clientId)
				return group;
		}
	}
	return nullptr;
}


GroupPtr Config::getGroupFromClient(ClientInfoPtr client)
{
	return getGroupFromClient(client->id);
}


json Config::getServerStatus(const json& streams) const
{
	Host host;
	host.update();
	//TODO: Set MAC and IP
	Snapserver snapserver("Snapserver", VERSION);
	json serverStatus = {
		{"server", {
			{"host", host.toJson()},//getHostName()},
			{"snapserver", snapserver.toJson()}
		}},
		{"groups", getGroups()},
		{"streams", streams}
	};

	return serverStatus;
}



json Config::getGroups() const
{
	json result = json::array();
	for (auto group: groups)
		result.push_back(group->toJson());
	return result;
}


void Config::remove(ClientInfoPtr client)
{
	auto group = getGroupFromClient(client);
	if (!group)
		return;
	group->removeClient(client);
	if (group->empty())
		remove(group);
}


void Config::remove(GroupPtr group, bool force)
{
	if (!group)
		return;

	if (group->empty() || force)
		groups.erase(std::remove(groups.begin(), groups.end(), group), groups.end());
}

/*
GroupPtr Config::removeFromGroup(const std::string& groupId, const std::string& clientId)
{
	GroupPtr group = getGroup(groupId);
	if (!group || (group->id != groupId))
		return group;

	auto client = getClientInfo(clientId);
	if (client)
		group->clients.erase(std::remove(group->clients.begin(), group->clients.end(), client), group->clients.end());

	addClientInfo(clientId);
	return group;
}


GroupPtr Config::setGroupForClient(const std::string& groupId, const std::string& clientId)
{
	GroupPtr oldGroup = getGroupFromClient(clientId);
	if (oldGroup && (oldGroup->id == groupId))
		return oldGroup;
	
	GroupPtr newGroup = getGroup(groupId);
	if (!newGroup)
		return nullptr;

	auto client = getClientInfo(clientId);
	if (!client)
		return nullptr;

	if (oldGroup)
		removeFromGroup(oldGroup->id, clientId);

	newGroup->addClient(client);
	return newGroup;	
}
*/


