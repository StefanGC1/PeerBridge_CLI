#include "logger.hpp"
#include <chrono>
#include <algorithm>
#include <regex>
#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/sinks/ConsoleSink.h>
#include <quill/sinks/FileSink.h>
#include <quill/sinks/RotatingFileSink.h>

namespace
{
using namespace std;

chrono::system_clock::time_point stringToTime(const string& datetimeString, const string& format)
{
    tm tmStruct = {};
    tmStruct.tm_isdst = -1;
    istringstream ss(datetimeString);
    ss >> get_time(&tmStruct, format.c_str());
    return chrono::system_clock::from_time_t(
        mktime(&tmStruct));
}

string timeToString(const chrono::system_clock::time_point& timePoint,
    const string& format)
{
    time_t time
        = chrono::system_clock::to_time_t(timePoint);
    tm* timeinfo = localtime(&time);
    char buffer[70];
    strftime(buffer, sizeof(buffer), format.c_str(),
             timeinfo);
    return buffer;
}

string isolateLastEntryInPath(const string& path)
{
    size_t found;
    found = path.find_last_of("/\\");

    return path.substr(found + 1);
}

string removeLastEntryInPath(const string& path)
{
    size_t found;
    found = path.find_last_of("/\\");

    return path.substr(0, found + 1);
}

string initializeLogDirectory()
{
    const string dateFormat = "%Y-%m-%d_%H-%M";
    const string currentTimeString = timeToString(chrono::system_clock::now(), dateFormat);
    const filesystem::path logsDir{"logs"};
    const filesystem::path currentLogDir{logsDir/currentTimeString};

    if (filesystem::exists(logsDir))
    {
        vector<chrono::system_clock::time_point> dirTimeList;
        for (auto const& dirEntry : filesystem::directory_iterator{logsDir})
        {
            const string isolatedFolderName = isolateLastEntryInPath(dirEntry.path().string());
            dirTimeList.push_back(stringToTime(isolatedFolderName, dateFormat));
        }

        if (dirTimeList.size() != 0 && dirTimeList.size() >= 6)
        {
            std::cout << "hello, this should be removed: " << timeToString(dirTimeList[0], dateFormat) << std::endl;
            sort(dirTimeList.begin(), dirTimeList.end());
            filesystem::remove_all(logsDir/timeToString(dirTimeList[0], dateFormat));
        }
    }
    else
    {
        if (!filesystem::create_directory(logsDir));
            cerr << "[ERROR] Creating log folder failed, stopping process..." << endl;
    }

    filesystem::create_directory(currentLogDir);
    return currentLogDir.string();
}

quill::ConsoleSinkConfig dimConsoleColours()
{
    using namespace quill;
    ConsoleSinkConfig cfg;

    ConsoleSinkConfig::Colours c;
    c.apply_default_colours();
    using CC = ConsoleSinkConfig::Colours;

    c.assign_colour_to_log_level(LogLevel::Warning,
                                 "\033[2m\033[33m");
    c.assign_colour_to_log_level(LogLevel::Error,
                                 "\033[2m\033[31m");
    c.assign_colour_to_log_level(LogLevel::Critical,
                                 "\033[2m\033[31m");

    cfg.set_colours(std::move(c));
    cfg.set_colour_mode(ConsoleSinkConfig::ColourMode::Always);
    return cfg;
}

void cmakeMoment(std::string& str)
{
    for (auto& c : str)
    {
        char prefferedSeparator = filesystem::path::preferred_separator;
        // 😭😭😭😭
        if (c == '/' || c == 92) c = filesystem::path::preferred_separator;
    }
}

quill::PatternFormatterOptions shortLogFormat()
{
    using quill::PatternFormatterOptions;
    PatternFormatterOptions opt{
        "%(time) [%(thread_id)] %(source_location) "
        "%(log_level) %(message)"
    };

    std::string prefix = SOURCE_ROOT_DIR;
    cmakeMoment(prefix);
    std::cout << prefix << std::endl;
    opt.source_location_path_strip_prefix = prefix;
    return opt;
}
}

static quill::Logger* sysLogObject{};
static quill::Logger* netLogObject{};

void initLogging(bool shouldLogTraffic)
{
    shouldLogNetTraffic = shouldLogTraffic;
    const std::string logsPath = initializeLogDirectory();

    using namespace quill;

    // Configure the backend thread to sleep instead of busy-spinning
    BackendOptions cfg;
    cfg.sleep_duration = std::chrono::milliseconds(1);
    Backend::start(cfg);

    auto consoleSink = Frontend::create_or_get_sink<ConsoleSink>("console", dimConsoleColours());
    auto sysSink = Frontend::create_or_get_sink<FileSink>(
        logsPath + "/app.log",
        []
        {
            FileSinkConfig cfg;
            cfg.set_open_mode('w');
            return cfg;
        }
        (),
        FileEventNotifier{});
    auto netSink = Frontend::create_or_get_sink<RotatingFileSink>(
        logsPath + "/net.log",
        []
        {
            RotatingFileSinkConfig cfg;
            cfg.set_open_mode('w');
            
            cfg.set_rotation_max_file_size(5 * 1024 * 1024); // 5 Mb
            cfg.set_rotation_naming_scheme(
                RotatingFileSinkConfig::RotationNamingScheme::DateAndTime);
            return cfg;
        }());

    sysLogObject = Frontend::create_or_get_logger("app", {consoleSink, sysSink}, shortLogFormat());
    netLogObject = Frontend::create_or_get_logger("net", netSink, shortLogFormat());
}

quill::Logger* sysLogger() { return sysLogObject; }
quill::Logger* netLogger() { return netLogObject; }
