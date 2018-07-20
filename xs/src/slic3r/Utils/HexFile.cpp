#include "HexFile.hpp"

#include <iostream>   // XXX
#include <sstream>
#include <boost/filesystem/fstream.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ini_parser.hpp>

namespace fs = boost::filesystem;
namespace pt = boost::property_tree;


namespace Slic3r {
namespace Utils {


static HexFile::DeviceKind parse_device_kind(const std::string &str)
{
	     if (str == "mk2") { return HexFile::DEV_MK2; }
	else if (str == "mk3") { return HexFile::DEV_MK3; }
	else if (str == "mm-control") { return HexFile::DEV_MM_CONTROL; }
	else { return HexFile::DEV_GENERIC; }
}

HexFile::HexFile(fs::path path) :
	path(std::move(path)),
	device(DEV_GENERIC)
{
	fs::ifstream file(this->path);
	if (! file.good()) {
		return;
	}

	std::string line;
	std::stringstream header_ini;
	while (std::getline(file, line, '\n').good()) {
		if (line.empty()) {
			continue;
		}

		// Account for LF vs CRLF
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}

		if (line.front() == ';') {
			line.front() = ' ';
			header_ini << line << std::endl;
		} else if (line.front() == ':') {
			break;
		}
	}

	pt::ptree ptree;
	try {
		pt::read_ini(header_ini, ptree);
	} catch (std::exception &e) {
		return;
	}

	const auto device = ptree.find("device");
	if (device != ptree.not_found()) {
		this->device = parse_device_kind(device->second.data());
	}

	const auto model_id = ptree.find("model_id");
	if (model_id != ptree.not_found()) {
		this->model_id = model_id->second.data();
	}

	std::cerr << "device: " << this->device << std::endl;
	std::cerr << "model_id: " << this->model_id << std::endl;
}


}
}
