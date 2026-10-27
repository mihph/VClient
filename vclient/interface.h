#pragma once
#include <string>
#include <boost/program_options.hpp>

namespace po = boost::program_options;

struct Params {
    std::string serverIp;
    int serverPort = 33333;
    std::string inputFile;
    std::string outputFile;
    std::string configFile;
};

class UserInterface {
private:
    po::options_description desc;
    po::variables_map vm;
    Params params;
public:
    UserInterface();
    bool parseArgs(int argc, char** argv);
    void showHelp();
    Params getParams() const { return params; }
};