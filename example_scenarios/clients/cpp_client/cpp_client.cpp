#include <cstdint>
#include <experimental/filesystem>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdlib.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cpp_client.h"
#include "json.hpp"

using namespace std;

using json = nlohmann::json;
namespace fs = experimental::filesystem;

// popen2 implementation adapted from:
// https://github.com/vi/syscall_limiter/blob/master/writelimiter/popen2.c
struct popen2 {
  pid_t child_pid;
  int from_child, to_child;
};

int popen2(const char *cmdline, struct popen2 *childinfo) {
  pid_t p;
  int pipe_stdin[2], pipe_stdout[2];

  if (pipe(pipe_stdin))
    return -1;
  if (pipe(pipe_stdout))
    return -1;

  printf("pipe_stdin[0] = %d, pipe_stdin[1] = %d\n", pipe_stdin[0],
         pipe_stdin[1]);
  printf("pipe_stdout[0] = %d, pipe_stdout[1] = %d\n", pipe_stdout[0],
         pipe_stdout[1]);

  p = fork();
  if (p < 0)
    return p;   /* Fork failed */
  if (p == 0) { /* child */
    close(pipe_stdin[1]);
    dup2(pipe_stdin[0], 0);
    close(pipe_stdout[0]);
    dup2(pipe_stdout[1], 1);
    execl("/bin/sh", "sh", "-c", cmdline, 0);
    perror("execl");
    exit(99);
  }
  childinfo->child_pid = p;
  childinfo->to_child = pipe_stdin[1];
  childinfo->from_child = pipe_stdout[0];
  return 0;
}

void fatalError(const string &msg) {
  cerr << "FATAL: " << msg << endl;
  exit(EXIT_FAILURE);
}

// Function that creates the json scenario for hypermapper
// Arguments:
// - AppName: Name of application
// - OutputFolderName: Name of output folder
// - NumIterations: Number of HP iterations
// - NumDSERandomSamples: Number of HP random samples
// - Predictor: Boolean for enabling/disabling feasibility predictor
// - InParams: vector of input parameters
// - Objectives: string with objective names
string createjson(string AppName, string OutputFoldername, int NumIterations,
                  int NumDSERandomSamples, bool Predictor,
                  vector<HMInputParam *> &InParams, vector<string> Objectives) {

  string CurrentDir = fs::current_path();
  string OutputDir = CurrentDir + "/" + OutputFoldername + "/";
  if (fs::exists(OutputDir)) {
    cerr << "Output directory exists, continuing!" << endl;
  } else {

    cerr << "Output directory does not exist, creating!" << endl;
    if (!fs::create_directory(OutputDir)) {
      fatalError("Unable to create Directory: " + OutputDir);
    }
  }
  json HMScenario;
  HMScenario["application_name"] = AppName;
  HMScenario["optimization_objectives"] = json(Objectives);
  HMScenario["hypermapper_mode"]["mode"] = "client-server";
  HMScenario["run_directory"] = CurrentDir;
  HMScenario["log_file"] = OutputFoldername + "/log_" + AppName + ".log";
  HMScenario["optimization_iterations"] = NumIterations;
  HMScenario["models"]["model"] = "random_forest";

  if (Predictor) {
    json HMFeasibleOutput;
    HMFeasibleOutput["enable_feasible_predictor"] = true;
    HMFeasibleOutput["false_value"] = "0";
    HMFeasibleOutput["true_value"] = "1";
    HMScenario["feasible_output"] = HMFeasibleOutput;
  }

  HMScenario["output_data_file"] =
      OutputFoldername + "/" + AppName + "_output_data.csv";
  HMScenario["output_pareto_file"] =
      OutputFoldername + "/" + AppName + "_output_pareto.csv";
  HMScenario["output_image"]["output_image_pdf_file"] =
      OutputFoldername + "_" + AppName + "_output_image.pdf";

  json HMDOE;
  HMDOE["doe_type"] = "standard latin hypercube"; // "random sampling";
  HMDOE["number_of_samples"] = NumDSERandomSamples;

  HMScenario["design_of_experiment"] = HMDOE;

  for (auto InParam : InParams) {
    json HMParam;
    HMParam["parameter_type"] = getTypeAsString(InParam->getType());
    switch (InParam->getType()) {
    case Ordinal:
    case Categorical:
    case Integer:
      HMParam["values"] = json(InParam->getRange());
      break;
    default:
      fatalError("Only Ordinal and Categorical handled!");
      break;
    }
    HMScenario["input_parameters"][InParam->getKey()] = HMParam;
  }

  //  cout << setw(4) << HMScenario << endl;
  ofstream HyperMapperScenarioFile;

  string JSonFileNameStr =
      CurrentDir + "/" + OutputFoldername + "/" + AppName + "_scenario.json";

  HyperMapperScenarioFile.open(JSonFileNameStr);
  if (HyperMapperScenarioFile.fail()) {
    fatalError("Unable to open file: " + JSonFileNameStr);
  }
  cout << "Writing JSON file to: " << JSonFileNameStr << endl;
  HyperMapperScenarioFile << setw(4) << HMScenario << endl;
  return JSonFileNameStr;
}

// Function that takes input parameters and generates objective
HMObjective calculateObjective(vector<HMInputParam *> &InputParams) {

  HMObjective Obj;
  int x1 = InputParams[0]->getVal();
  int x2 = InputParams[1]->getVal();

  Obj.f1_value = 2 + (x1 - 2) * (x1 - 2) + (x2 - 1) * (x2 - 1);
  Obj.f2_value = 9 * x1 - (x2 - 1) * (x2 - 1);

  bool c1 = ((x1 * x1 + x2 * x2) <= 255);
  bool c2 = ((x1 - 3 * x2 + 10) <= 0);
  Obj.valid = c1 && c2;
  return Obj;
}

// Function that populates input parameters
int collectInputParams(vector<HMInputParam *> &InParams) {
  int numParams = 0;

  vector<int> Range = {-20, 20};

  HMInputParam *x0Param = new HMInputParam("x0", ParamType::Integer);
  x0Param->setRange(Range);
  InParams.push_back(x0Param);
  numParams++;

  HMInputParam *x1Param = new HMInputParam("x1", ParamType::Integer);
  x1Param->setRange(Range);
  InParams.push_back(x1Param);
  numParams++;
  return numParams;
}

// Function for mapping input parameter based on key
auto findHMParamByKey(vector<HMInputParam *> &InParams, string Key) {
  for (auto it = InParams.begin(); it != InParams.end(); ++it) {
    HMInputParam Param = **it;
    if (Param == Key) {
      return it;
    }
  }
  return InParams.end();
}

int main(int argc, char **argv) {

  if (!getenv("HYPERMAPPER_HOME") || !getenv("PYTHONPATH")) {
    string ErrMsg = "Environment variables are not set!\n";
    ErrMsg += "Please set HYPERMAPPER_HOME and PYTHONPATH before running this ";
    fatalError(ErrMsg);
  }

  srand(0);

  vector<HMInputParam *> InParams;

  // Set these values accordingly
  // TODO: make these command line inputs
  string OutputFoldername = "outdata";
  string AppName = "cpp_chakong_haimes";
  int NumIterations = 20;
  int NumSamples = 10;
  bool Predictor = 1;
  vector<string> Objectives = {"f1_value", "f2_value"};

  // Create output directory if it doesn't exist
  string CurrentDir = fs::current_path();
  string OutputDir = CurrentDir + "/" + OutputFoldername + "/";
  if (fs::exists(OutputDir)) {
    cerr << "Output directory exists, continuing!" << endl;
  } else {

    cerr << "Output directory does not exist, creating!" << endl;
    if (!fs::create_directory(OutputDir)) {
      fatalError("Unable to create Directory: " + OutputDir);
    }
  }

  // Collect input parameters
  int numParams = collectInputParams(InParams);
  for (auto param : InParams) {
    cout << "Param: " << *param << "\n";
  }

  // Create json scenario
  string JSonFileNameStr =
      createjson(AppName, OutputFoldername, NumIterations, NumSamples,
                 Predictor, InParams, Objectives);

  // Launch HyperMapper
  string cmd("python3 ");
  cmd += getenv("HYPERMAPPER_HOME");
  cmd += "/scripts/hypermapper.py";
  cmd += " " + JSonFileNameStr;

  cout << "Executing command: " << cmd << endl;
  struct popen2 hypermapper;
  popen2(cmd.c_str(), &hypermapper);

  FILE *instream = fdopen(hypermapper.from_child, "r");
  FILE *outstream = fdopen(hypermapper.to_child, "w");

  const int max_buffer = 1000;
  char buffer[max_buffer];
  // Loop that communicates with HyperMapper
  // Everything is done through function calls,
  // there should be no need to modify bellow this line.
  int i = 0;
  while (true) {
    fgets(buffer, max_buffer, instream);
    cout << "Iteration: " << i << endl;
    cout << "Recieved: " << buffer;
    // Receiving Num Requests
    string bufferStr(buffer);
    if (!bufferStr.compare("End of HyperMapper\n")) {
      cout << "Hypermapper completed!\n";
      break;
    }
    string NumReqStr = bufferStr.substr(bufferStr.find(' ') + 1);
    int numRequests = stoi(NumReqStr);
    // Receiving input param names
    fgets(buffer, max_buffer, instream);
    bufferStr = string(buffer);
    cout << "Recieved: " << buffer;
    size_t pos = 0;
    // Create mapping for InputParam objects to keep track of order
    map<int, HMInputParam *> InputParamsMap;
    string response;
    for (int param = 0; param < numParams; param++) {
      size_t len = bufferStr.find_first_of(",\n", pos) - pos;
      string ParamStr = bufferStr.substr(pos, len);
      //      cout << "  -- param: " << ParamStr << "\n";
      auto paramIt = findHMParamByKey(InParams, ParamStr);
      if (paramIt != InParams.end()) {
        InputParamsMap[param] = *paramIt;
        response += ParamStr;
        response += ",";
      } else {
        fatalError("Unknown parameter received!");
      }
      pos = bufferStr.find_first_of(",\n", pos) + 1;
    }
    for (auto objString : Objectives)
      response += objString + ",";
    if (Predictor)
      response += "Valid";
    response += "\n";
    // For each request
    for (int request = 0; request < numRequests; request++) {
      // Receiving paramter values
      fgets(buffer, max_buffer, instream);
      cout << "Received: " << buffer;
      bufferStr = string(buffer);
      pos = 0;
      for (int param = 0; param < numParams; param++) {
        size_t len = bufferStr.find_first_of(",\n", pos) - pos;
        string ParamValStr = bufferStr.substr(pos, len);
        InputParamsMap[param]->setVal(stoi(ParamValStr));
        response += ParamValStr;
        response += ",";
        pos = bufferStr.find_first_of(",\n", pos) + 1;
      }
      HMObjective Obj = calculateObjective(InParams);
      response += to_string(Obj.f1_value);
      response += ",";
      response += to_string(Obj.f2_value);
      response += ",";
      response += to_string(Obj.valid);
      response += "\n";
    }
    cout << "Response:\n" << response;
    fputs(response.c_str(), outstream);
    fflush(outstream);
    i++;
  }

  close(hypermapper.from_child);
  close(hypermapper.to_child);

  FILE *fp;
  string cmdPareto("python3 ");
  cmdPareto += getenv("HYPERMAPPER_HOME");
  cmdPareto += "/scripts/compute_pareto.py";
  cmdPareto += " " + JSonFileNameStr;
  cout << "Executing " << cmdPareto << endl;
  fp = popen(cmdPareto.c_str(), "r");
  while (fgets(buffer, max_buffer, fp))
    printf("%s", buffer);
  pclose(fp);

  return 0;
}

int HMInputParam::count = 0;
