// Generated from the EQUITIES table of euronext_tracker.py
// (category "photovoltaic " trimmed of its trailing space).

#include <string.h>
#include "equities.h"

const Equity EQUITIES[] = {
    { "AL2SI.PA",  { "technology", "servers", "hardware" } },
    { "AALB.AS",   { "industrials", "flow control", "climate technology" } },
    { "ABN.AS",    { "banking", "financials" } },
    { "AC.PA",     { "hospitality", "hotels", "real estate" } },
    { "AIR.PA",    { "aerospace", "defense", "aviation" } },
    { "ALFEN.AS",  { "energy", "EV charging", "smart grid" } },
    { "ATE.PA",    { "technology", "semiconductors", "automotive" } },
    { "AMG.AS",    { "materials", "specialty metals", "energy storage" } },
    { "GLDM.PA",   { "gold", "commodities", "ETF" } },
    { "AMUN.PA",   { "financials", "asset management", "ETF" } },
    { "AKE.PA",    { "chemicals", "specialty materials", "industrials" } },
    { "ASY.PA",    { "technology", "cybersecurity", "IT services" } },
    { "AM.PA",     { "aerospace", "defense", "aviation" } },
    { "DEME.BR",   { "marine engineering", "offshore", "infrastructure" } },
    { "EMEIS.PA",  { "healthcare", "elder care", "nursing homes" } },
    { "ENRGY.BR",  { "energy", "renewables", "Belgium" } },
    { "ENGI.PA",   { "utilities", "energy", "infrastructure" } },
    { "EL.PA",     { "luxury goods", "cosmetics", "retail" } },
    { "ETL.PA",    { "Telecommunications", "aerospace" } },
    { "EXA.PA",    { "defense", "robotic", "marine" } },
    { "EXENS.PA",  { "defense", "optronic", "detection" } },
    { "FER.AS",    { "infrastructure", "construction", "Netherlands" } },
    { "PERR.PA",   { "industrial", "nuclear" } },
    { "ALAIR.PA",  { "consumer", "air-conditioning" } },
    { "ALOKW.PA",  { "energy", "photovoltaic", "France" } },
    { "MLHK.PA",   { "industrials", "small cap", "France" } },
    { "NK.PA",     { "materials", "mines" } },
    { "JCQ.PA",    { "industrials", "engineering", "France" } },
    { "ALKAL.PA",  { "technology", "software", "small cap" } },
    { "AI.PA",     { "industrials", "gases", "chemicals" } },
    { "LBIRD.PA",  { "defense", "drones", "aerospace" } },
    { "MRN.PA",    { "marine", "shipping", "industrials" } },
    { "NANO.PA",   { "technology", "nanotechnology", "materials" } },
    { "NEDAP.AS",  { "technology", "IoT", "identification systems" } },
    { "OVH.PA",    { "cloud computing", "hosting", "technology" } },
    { "PARRO.PA",  { "technology", "AI", "voice recognition" } },
    { "PEUG.PA",   { "automotive", "manufacturing", "mobility" } },
    { "ALRIB.PA",  { "technology", "small cap", "France" } },
    { "SAF.PA",    { "aerospace", "defense", "aviation engines" } },
    { "SU.PA",     { "energy management", "electricals", "industrials" } },
    { "GLE.PA",    { "banking", "financials", "investment banking" } },
    { "SW.PA",     { "industrials", "pumps", "fluid systems" } },
    { "SOI.PA",    { "materials", "insulation", "construction" } },
    { "STLAP.PA",  { "automotive", "manufacturing", "mobility" } },
    { "ALSTI.PA",  { "technology", "small cap", "France" } },
    { "STMPA.PA",  { "semiconductors", "technology", "automotive" } },
    { "TE.PA",     { "telecoms", "fiber", "infrastructure" } },
    { "HO.PA",     { "defense", "aerospace", "electronics" } },
    { "TTE.PA",    { "energy", "oil & gas", "renewables" } },
    { "UBI.PA",    { "gaming", "entertainment", "software" } },
    { "UMI.BR",    { "materials", "chemicals", "recycling" } },
    { "VIL.PA",    { "finance", "shares" } },
    { "WAGA.PA",   { "energy", "biogas", "renewables" } },
    { "BRNT.PA",   { "energy", "oil", "commodities" } },
};

const int EQUITY_COUNT = sizeof(EQUITIES) / sizeof(EQUITIES[0]);

const Equity* FindEquity(const char* symbol)
{
    for (int i = 0; i < EQUITY_COUNT; ++i)
    {
        if (strcmp(EQUITIES[i].symbol, symbol) == 0)
            return &EQUITIES[i];
    }
    return nullptr;
}
