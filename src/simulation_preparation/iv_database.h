#pragma once

#include "pv_voltage_iv_curve.h"
#include <sqlite3.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>

// Module curves are stored as little-endian int32 count, V/Voc[], I/Isc[].
// Interpolate normalized curves on the actual grid, then find their maximum power.
class IVDatabase {
    struct Curve { double voc, isc; std::vector<double> v, i; };
    std::map<std::pair<double, double>, Curve> curves_;
    std::set<double> irradiances_, temperatures_;
    static std::pair<double, double> bracket(const std::set<double>& axis, double x) {
        auto hi = axis.lower_bound(x);
        if (hi == axis.end()) return {*axis.rbegin(), *axis.rbegin()};
        if (hi == axis.begin() || *hi == x) return {*hi, *hi};
        return {*std::prev(hi), *hi};
    }
    static double current(const Curve& c, double v) {
        auto hi = std::lower_bound(c.v.begin(), c.v.end(), v);
        if (hi == c.v.begin()) return c.i.front();
        if (hi == c.v.end()) return c.i.back();
        const auto k = static_cast<std::size_t>(hi - c.v.begin());
        const double w = (v - c.v[k-1]) / (c.v[k] - c.v[k-1]);
        return c.i[k-1] * (1-w) + c.i[k] * w;
    }
public:
    IVDatabase(const std::string& path, const std::string& part) {
        sqlite3* raw = nullptr;
        const int opened = sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr);
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
        if (opened != SQLITE_OK) throw std::runtime_error("Cannot open IV database: " + path);
        sqlite3_stmt* query = nullptr;
        const char* sql = "SELECT Irradiance,Temperature,Voc,Isc,ShapeData FROM pv_performance_maps WHERE PartNumber=?";
        if (sqlite3_prepare_v2(raw, sql, -1, &query, nullptr) != SQLITE_OK)
            throw std::runtime_error("IV database has no readable pv_performance_maps table");
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(query, sqlite3_finalize);
        sqlite3_bind_text(query, 1, part.c_str(), -1, SQLITE_TRANSIENT);
        int result;
        while ((result = sqlite3_step(query)) == SQLITE_ROW) {
            const double g = sqlite3_column_double(query,0), t = sqlite3_column_double(query,1);
            Curve c{sqlite3_column_double(query,2), sqlite3_column_double(query,3), {}, {}};
            const auto* data = static_cast<const unsigned char*>(sqlite3_column_blob(query,4));
            const int bytes = sqlite3_column_bytes(query,4);
            if (!data || bytes < 4) throw std::runtime_error("Invalid IV shape blob");
            const std::uint32_t n = std::uint32_t(data[0]) | (std::uint32_t(data[1]) << 8) |
                (std::uint32_t(data[2]) << 16) | (std::uint32_t(data[3]) << 24);
            if (n < 2 || n > 10000 || bytes != 4 + 16*static_cast<int>(n) ||
                !std::isfinite(g) || g <= 0 || !std::isfinite(t) ||
                !std::isfinite(c.voc) || c.voc <= 0 || !std::isfinite(c.isc) || c.isc <= 0)
                throw std::runtime_error("Invalid IV database record");
            for (std::uint32_t j=0; j<2*n; ++j) {
                std::uint64_t bits=0;
                for (int k=0;k<8;++k) bits |= std::uint64_t(data[4+8*j+k]) << (8*k);
                double value; std::memcpy(&value, &bits, 8);
                if (!std::isfinite(value) || value < -1e-8 || value > 1.001)
                    throw std::runtime_error("Invalid normalized IV value");
                (j<n ? c.v : c.i).push_back(value);
            }
            for (std::size_t k=1;k<c.v.size();++k)
                if (c.v[k] <= c.v[k-1] || c.i[k] > c.i[k-1]+1e-6)
                    throw std::runtime_error("IV curve is not monotonic");
            if (std::abs(c.v.front()) > 1e-6 || std::abs(c.v.back()-1) > 1e-6 ||
                std::abs(c.i.back()) > 0.01)
                throw std::runtime_error("Invalid IV endpoints; regenerate module IV database");
            if (part == "CS6U-330P" && g == 1000 && t == 25 &&
                (c.voc < 40 || c.voc > 50))
                throw std::runtime_error("CS6U-330P STC Voc inconsistent with 45.6 V module; regenerate IV database");
            curves_[{g,t}] = std::move(c); irradiances_.insert(g); temperatures_.insert(t);
        }
        if (result != SQLITE_DONE || curves_.empty()) throw std::runtime_error("No complete IV data for " + part);
        if (curves_.size() != irradiances_.size()*temperatures_.size())
            throw std::runtime_error("IV database grid has missing corners");
    }
    IVCurveData lookup(double g, double t, int series, int parallel) const {
        if (!std::isfinite(g) || !std::isfinite(t) || g <= 0 || series <= 0 || parallel <= 0)
            throw std::runtime_error("Invalid IV lookup inputs");
        // Do not silently substitute the boundary curve outside the database coverage.
        if (g < *irradiances_.begin() || g > *irradiances_.rbegin() ||
            t < *temperatures_.begin() || t > *temperatures_.rbegin())
            throw std::runtime_error("IV lookup outside database grid: G=" + std::to_string(g) + " T=" + std::to_string(t));
        auto gs=bracket(irradiances_,g), ts=bracket(temperatures_,t);
        const double wg=gs.first==gs.second ? 0 : (g-gs.first)/(gs.second-gs.first);
        const double wt=ts.first==ts.second ? 0 : (t-ts.first)/(ts.second-ts.first);
        const Curve* corners[4] = {&curves_.at({gs.first,ts.first}), &curves_.at({gs.second,ts.first}),
                                  &curves_.at({gs.first,ts.second}), &curves_.at({gs.second,ts.second})};
        const double weights[4] = {(1-wg)*(1-wt),wg*(1-wt),(1-wg)*wt,wg*wt};
        IVCurveData out;
        for(int k=0;k<4;++k) { out.voc += weights[k]*corners[k]->voc; out.isc += weights[k]*corners[k]->isc; }
        double best=-1;
        // Piecewise-linear interpolated I(V): power is quadratic on each segment.
        std::set<double> knots{0,1};
        for (auto c:corners) knots.insert(c->v.begin(),c->v.end());
        auto norm_i = [&](double v) { double i=0; for(int k=0;k<4;++k) i+=weights[k]*current(*corners[k],v); return i; };
        auto consider = [&](double v,double i) { if(v*i>best) {best=v*i;out.pv_voltage=v*out.voc;out.pv_current=i*out.isc;} };
        double prev=0, pi=norm_i(0);
        for(double v:knots) {
            const double i=norm_i(v); consider(v,i);
            if(v>prev) {
                const double slope=(i-pi)/(v-prev), intercept=pi-slope*prev;
                if(slope<0) { const double peak=-intercept/(2*slope); if(peak>prev && peak<v) consider(peak,intercept+slope*peak); }
            }
            prev=v;pi=i;
        }
        out.voc*=series; out.pv_voltage*=series; out.isc*=parallel; out.pv_current*=parallel;
        out.valid=out.pv_voltage>0 && out.pv_current>0;
        return out;
    }
};
