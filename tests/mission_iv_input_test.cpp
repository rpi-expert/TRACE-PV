#include "simulation_preparation/mission_profile_loader.h"
#include "simulation_preparation/iv_database.h"
#include "simulation_model.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <chrono>

void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
int main() {
    auto dir=std::filesystem::temp_directory_path()/ ("tracepv-input-test-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(dir);
    {
        std::ofstream env(dir/"env.csv");
        env << "time,ambient_temperature,rh,GHI\na,-1,50,500\nb,-5,50,500\nc,20,0.1,500\nd,bad,50,500\ne,25,55,1000\nf,NaN,50,100\ng,20,101,500\nh,20,1,500\ni,20,1.01,500\n";
        std::ofstream op(dir/"op.csv");
        op << "time,ac_voltage\ne,480\nd,480\nc,480\nb,470\na,480\nf,480\ng,480\nh,480\ni,480\n";
    }
    auto filtered=load_mission_profile((dir/"env.csv").string(),(dir/"op.csv").string());
    require(filtered.size()==3,"sentinel/RH/finite filtering");
    require(filtered[0].time=="b" && filtered[0].ambient_temperature==-5 && filtered[0].ac_voltage==470,"timestamp alignment and valid subzero input");
    require(filtered[1].time=="e" && filtered[1].solar_irradiance==1000,"parse failure must not shift following rows");
    {
        std::ofstream combined(dir/"combined.csv");
        combined << "time,ambient_temperature,rh,GHI,ac_voltage\na,-1,50,500,480\nb,20,0.01,500,480\nc,-5,60,500,480\nd,20,50,nan,480\n";
    }
    require(load_mission_profile_csv((dir/"combined.csv").string()).size()==1,"combined input filtering");
    SimulationModel model;
    require(load_simulation_model("simulator_inputs/simulation_model/example_simulation_model.json",model),"model loads");
    require(model.pv_modules_per_string==18 && model.pv_parallel_strings==6,"array configuration");
    IVDatabase db(model.iv_database_path,model.pv_panel_part_number);
    auto stc=db.lookup(1000,25,1,1);
    require(stc.valid && std::abs(stc.voc-45.6)<0.1 && std::abs(stc.pv_voltage*stc.pv_current-330)<2,"module STC validation");
    auto array=db.lookup(1000,25,18,6);
    require(std::abs(array.pv_voltage/stc.pv_voltage-18)<1e-9 && std::abs(array.pv_current/stc.pv_current-6)<1e-9,"array scaling");
    auto mid=db.lookup(975,22.5,1,1);
    double expected=0;
    for(double g:{950.,1000.}) for(double t:{20.,25.}) expected+=db.lookup(g,t,1,1).voc/4;
    require(std::abs(mid.voc-expected)<1e-9,"bilinear grid interpolation");
    require(db.lookup(0.01,-45,1,1).valid && db.lookup(1600,105,1,1).valid,"exact grid edges");
    bool rejected=false;
    try { db.lookup(2000,25,1,1); } catch(const std::exception&) { rejected=true; }
    require(rejected,"out-of-range must be explicit");
    rejected=false;
    try { IVDatabase invalid("component_database/component_parameters.db","CS6U-330P"); } catch(const std::exception&) { rejected=true; }
    require(rejected,"legacy corrupt database must not silently run");
    std::vector<SimulationCase> year;
    if (std::filesystem::exists("simulator_inputs/mission_profile/environmental_condition/environmental_mission_profile.csv"))
        year=load_mission_profile("simulator_inputs/mission_profile/environmental_condition/environmental_mission_profile.csv","simulator_inputs/mission_profile/operating_condition/operating_mission_profile.csv");
    for(const auto& sc:year) require(db.lookup(sc.solar_irradiance,sc.ambient_temperature,18,6).valid,"full-year IV coverage");
    std::cout << "PASS: input cleaning, timestamp alignment, IV interpolation/STC/edges/scaling; full-year cases=" << year.size() << '\n';
    std::filesystem::remove_all(dir);
}
