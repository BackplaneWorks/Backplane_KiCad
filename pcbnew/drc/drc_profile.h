/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

// Measurement probe for DRC and zone filling: wall time, process CPU time (so effective
// cores = cpu / wall) and resident memory around a scope.  Enabled only when the
// KICAD_DRC_PROFILE environment variable is set; prints one line to stderr per scope.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#if defined( __linux__ ) || defined( __APPLE__ )
#include <sys/resource.h>
#include <unistd.h>
#endif

class DRC_PROFILE_PROBE
{
public:
    explicit DRC_PROFILE_PROBE( std::string aName ) :
            m_name( std::move( aName ) ),
            m_enabled( std::getenv( "KICAD_DRC_PROFILE" ) != nullptr )
    {
        if( !m_enabled )
            return;

        m_start = std::chrono::steady_clock::now();
        m_cpuStart = cpuMs();
        m_rssStart = rssKb();
    }

    ~DRC_PROFILE_PROBE()
    {
        if( !m_enabled )
            return;

        double wall = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now()
                                                                 - m_start ).count();
        double cpu = cpuMs() - m_cpuStart;
        long   rss = rssKb();

        std::fprintf( stderr, "DRCPROF\t%s\twall_ms=%.1f\tcpu_ms=%.1f\tcores=%.2f\trss_kb=%ld\t"
                              "drss_kb=%ld\tmaxrss_kb=%ld\n",
                      m_name.c_str(), wall, cpu, wall > 0.0 ? cpu / wall : 0.0, rss,
                      rss - m_rssStart, maxRssKb() );
    }

private:
    static double cpuMs()
    {
#if defined( __linux__ ) || defined( __APPLE__ )
        rusage ru;
        getrusage( RUSAGE_SELF, &ru );
        return ( ru.ru_utime.tv_sec + ru.ru_stime.tv_sec ) * 1000.0
               + ( ru.ru_utime.tv_usec + ru.ru_stime.tv_usec ) / 1000.0;
#else
        return 0.0;
#endif
    }

    static long maxRssKb()
    {
#if defined( __linux__ )
        rusage ru;
        getrusage( RUSAGE_SELF, &ru );
        return ru.ru_maxrss;
#else
        return 0;
#endif
    }

    static long rssKb()
    {
#if defined( __linux__ )
        long  pages = 0, resident = 0;
        FILE* f = std::fopen( "/proc/self/statm", "r" );

        if( f )
        {
            if( std::fscanf( f, "%ld %ld", &pages, &resident ) != 2 )
                resident = 0;

            std::fclose( f );
        }

        return resident * ( sysconf( _SC_PAGESIZE ) / 1024 );
#else
        return 0;
#endif
    }

    std::string                           m_name;
    bool                                  m_enabled;
    std::chrono::steady_clock::time_point m_start;
    double                                m_cpuStart = 0.0;
    long                                  m_rssStart = 0;
};
