#include "StandardsCatalog.h"

#include "AgentJson.h"
#include <QJsonArray>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

namespace {
std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool containsInsensitive(const std::string& value, const std::string& needle) {
    if (needle.empty()) return true;
    return lowerAscii(value).find(lowerAscii(needle)) != std::string::npos;
}

void addFinding(Model::StandardApplicabilityResult& result,
                const char* code, const char* severity, const char* message) {
    result.findings.push_back({code, severity, message});
}

void requireEvidence(Model::StandardApplicabilityResult& result, const char* evidence) {
    if (std::find(result.requiredEvidence.begin(), result.requiredEvidence.end(), evidence)
        == result.requiredEvidence.end())
        result.requiredEvidence.emplace_back(evidence);
}
}

namespace Model {

const std::vector<StandardCatalogEntry>& standardCatalog() {
    static const std::vector<StandardCatalogEntry> entries = {
        {"legacy-iso10816-class-i-iv/1.0.0", "DPA", "DPA legacy ISO 10816 Class I-IV compatibility table", "execution-approved", "Project compatibility only; not an ISO 20816 applicability decision", "execution-approved", "project-owned-derived-rule", "", true},
        {"iso-20816-1:2016", "ISO", "Mechanical vibration — measurement and evaluation of machine vibration — Part 1", "published", "General rotating/non-rotating, non-reciprocating machine vibration", "catalog_only", "purchase_or_authorized_copy", "https://www.iso.org/standard/63180.html", true},
        {"iso-20816-3:2022", "ISO", "Mechanical vibration — Part 3: industrial machinery above 15 kW", "published", "In-situ industrial machinery in the stated power/speed range", "catalog_only", "purchase_or_authorized_copy", "https://www.iso.org/standard/78311.html", true},
        {"iso-13373-2:2016", "ISO", "Condition monitoring — vibration data processing, analysis and presentation", "published", "Vibration data processing and reporting", "catalog_only", "purchase_or_authorized_copy", "https://www.iso.org/standard/68128.html", true},
        {"iso-13373-3:2015", "ISO", "Condition monitoring — guidelines for vibration diagnosis", "published", "Machine vibration diagnosis", "catalog_only", "purchase_or_authorized_copy", "https://www.iso.org/standard/40840.html", true},
        {"iso-17359:2018", "ISO", "Condition monitoring and diagnostics — general guidelines", "published", "Condition monitoring programme design for machines", "catalog_only", "purchase_or_authorized_copy", "https://www.iso.org/standard/71194.html?browse=ics", true},
        {"iso-5348:2021", "ISO", "Mechanical vibration — mechanical mounting of accelerometers", "published", "Accelerometer mounting and frequency-response fidelity", "catalog_only", "purchase_or_authorized_copy", "https://www.iso.org/standard/78160.html", true},
        {"iso-2954:2012", "ISO", "Mechanical vibration — requirements for instruments measuring vibration severity", "published", "Vibration severity instrument capability", "catalog_only", "purchase_or_authorized_copy", "https://www.iso.org/standard/21835.html", true},
        {"iso-13374-1:2003", "ISO", "Condition monitoring — data processing, communication and presentation", "published", "Condition monitoring software data model", "catalog_only", "purchase_or_authorized_copy", "https://www.iso.org/cms/%20render/live/en/sites/isoorg/contents/data/standard/02/18/21832.html", true},
        {"iso-13374-3:2012", "ISO", "Condition monitoring and diagnostics of machines — data processing, communication and presentation — Part 3", "published", "Open condition-monitoring data communication and interoperability", "catalog_only", "purchase_or_authorized_copy", "https://www.iso.org/standard/37611.html", true},
        {"iso-18436-2:2014", "ISO", "Condition monitoring — qualification and assessment of vibration personnel", "published", "Personnel competence categories for vibration diagnostics", "catalog_only", "purchase_or_authorized_copy", "https://www.iso.org/standard/50447.html?browse=ics", true},
        {"iso-21940-11:2016", "ISO", "Mechanical vibration — Rotor balancing — Part 11: rigid rotors", "published", "Procedures and tolerances for rotors with rigid behaviour", "catalog_only", "purchase_or_authorized_copy", "https://www.iso.org/standard/54074.html", true},
        {"iso-21940-12:2016", "ISO", "Mechanical vibration — Rotor balancing — Part 12: flexible rotors", "published", "Procedures and tolerances for rotors with flexible behaviour", "catalog_only", "purchase_or_authorized_copy", "https://www.iso.org/standard/50429.html", true},
        {"iso-21940-13:2012", "ISO", "Rotor balancing — in-situ balancing of medium and large rotors", "published", "In-situ balancing of medium/large rotors", "catalog_only", "purchase_or_authorized_copy", "https://www.iso.org/standard/56561.html", true},
        {"iso-21940-21:2022", "ISO", "Mechanical vibration — Rotor balancing — Part 21: balancing machine evaluation", "published", "Description and evaluation of hard- and soft-bearing balancing machines", "catalog_only", "purchase_or_authorized_copy", "https://www.iso.org/standard/78146.html", true},
        {"iso-16063-13:2001", "ISO", "Methods for calibration of vibration and shock transducers — Part 13", "published", "Primary shock calibration using laser interferometry", "catalog_only", "purchase_or_authorized_copy", "https://www.iso.org/standard/27075.html", true},
        {"iso-16063-17:2016", "ISO", "Methods for calibration of vibration and shock transducers — Part 17", "published", "Primary accelerometer calibration by centrifuge", "catalog_only", "purchase_or_authorized_copy", "https://www.iso.org/standard/65672.html", true},
        {"iso-16063-21:2003", "ISO", "Methods for calibration of vibration and shock transducers — Part 21", "published", "Vibration calibration by comparison to a reference transducer", "catalog_only", "purchase_or_authorized_copy", "https://www.iso.org/standard/27053.html", true},
        {"iso-16063-32:2016", "ISO", "Methods for calibration of vibration and shock transducers — Part 32", "published", "Accelerometer frequency/phase response by shock excitation", "catalog_only", "purchase_or_authorized_copy", "https://www.iso.org/standard/40382.html", true},
        {"gb-t-6075.1:2012", "GB/T", "机械振动 在非旋转部件上测量和评价机器的机械振动 第1部分：总则", "current", "General machine vibration measurement/evaluation", "catalog_only", "official_catalog_or_authorized_copy", "https://std.samr.gov.cn/bzgk/std/newGbInfo?hcno=C6D9EEB23053650AF6667535FD2E33EF", true},
        {"gb-t-6075.3:2011", "GB/T", "机械振动 第3部分：额定功率大于15 kW的工业机器", "current", "Industrial machines by power/speed class", "catalog_only", "official_catalog_or_authorized_copy", "https://openstd.samr.gov.cn/bzgk/std/std_list?p.p1=0&p.p2=GB%2FT+6075.&p.p90=circulation_date&p.p91=desc", true},
        {"gb-t-6075.4:2015", "GB/T", "机械振动 在非旋转部件上测量评价机器的振动 第4部分：具有滑动轴承的燃气轮机组", "current", "Gas turbine sets with fluid-film bearings", "catalog_only", "official_catalog_or_authorized_copy", "https://std.samr.gov.cn/gb/search/gbDetailed?id=71F772D79054D3A7E05397BE0A0AB82A", true},
        {"gb-t-6075.6:2024", "GB/T", "机械振动 第6部分：功率大于100 kW的往复式机器", "current", "Reciprocating machines above 100 kW", "catalog_only", "official_catalog_or_authorized_copy", "https://std.samr.gov.cn/bzgk/std/newGbInfo?hcno=2F7F3E82D29C2BF430F02C83CAD034EC", true},
        {"gb-t-6075.7:2015", "GB/T", "机械振动 在非旋转部件上测量评价机器的振动 第7部分：工业应用的旋转动力泵", "current", "Industrial rotodynamic pumps including rotating-shaft measurements", "catalog_only", "official_catalog_or_authorized_copy", "https://std.samr.gov.cn/gb/search/gbDetailed?id=71F772D80FB2D3A7E05397BE0A0AB82A", true},
        {"gb-t-19873-series", "GB/T", "机械振动与冲击 机器状态监测系列", "current", "Monitoring, processing and diagnostics", "catalog_only", "official_catalog_or_authorized_copy", "https://openstd.samr.gov.cn/bzgk/std/std_list?p.p1=0&p.p2=GBT19873&p.p90=circulation_date&p.p91=desc", true},
        {"gb-t-2423.10:2019", "GB/T", "环境试验 试验Fc：振动（正弦）", "current", "Environmental sinusoidal vibration testing", "catalog_only", "official_catalog_or_authorized_copy", "https://std.samr.gov.cn/bzgk/std/newGbInfo?hcno=CC5557EE457ACFBDE5999DD4D12AA826", true},
        {"gb-t-2423.5:2019", "GB/T", "环境试验 试验Ea：冲击", "current", "Environmental shock testing", "catalog_only", "official_catalog_or_authorized_copy", "https://openstd.samr.gov.cn/bzgk/std/newGbInfo?hcno=8FB22EB67603CA59BD25A5CEA7D1AE9D", true},
        {"gb-t-2423.56:2023", "GB/T", "环境试验 试验Fh：宽带随机振动和导则", "current", "Broadband random vibration testing", "catalog_only", "official_catalog_or_authorized_copy", "https://openstd.samr.gov.cn/bzgk/std/newGbInfo?hcno=A4EB05C82594C1AE41D3B82869E7FB87", true},
        {"gb-t-2423.62:2018", "GB/T", "环境试验 试验Fi：混合模式多激励振动", "current", "MIMO/multi-exciter vibration testing", "catalog_only", "official_catalog_or_authorized_copy", "https://openstd.samr.gov.cn/bzgk/std/newGbInfo?hcno=882F3B736EC244DCD3EF879803C3F27B", true},
        {"gb-t-9239.11:2025", "GB/T", "机械振动 转子平衡 第11部分：刚性转子", "current", "Rigid rotor balancing", "catalog_only", "official_catalog_or_authorized_copy", "https://openstd.samr.gov.cn/bzgk/std/newGbInfo?hcno=2645EC6BADB20E8509B9E07B5B396F90", true},
        {"gb-t-9239.21:2019", "GB/T", "机械振动 转子平衡 第21部分：平衡机的描述与评定", "current", "Description and evaluation of balancing machines", "catalog_only", "official_catalog_or_authorized_copy", "https://std.samr.gov.cn/gb/search/gbDetailed?id=95A47695C4EB4F2CE05397BE0A0AB3E0", true},
        {"mil-std-810:2022", "MIL-STD", "Environmental Engineering Considerations and Laboratory Tests", "active", "Tailored environmental engineering and laboratory test methods", "catalog_only", "official_dla_catalog", "https://quicksearch.dla.mil/qsDocDetails.aspx?ident_number=35978", true},
        {"mil-std-167-1a:2022", "MIL-STD", "Mechanical Vibrations of Shipboard Equipment", "active", "Shipboard equipment vibration", "catalog_only", "official_dla_catalog_or_authorized_copy", "https://quicksearch.dla.mil/Transient/02AD7456E5964C5E96A779A57655434A.pdf", true},
        {"di-envr-81647", "DI", "Mechanical Vibrations of Shipboard Equipment Measurement Test Data", "active", "Shipboard vibration test plan/report data items", "catalog_only", "official_dla_catalog", "https://quicksearch.dla.mil/qsDocDetails.aspx?ident_number=215805", true},
        {"mil-dtl-901e:2023", "MIL-DTL", "High-impact shipboard machinery, equipment and systems shock tests", "active", "High-impact shipboard shock survivability", "catalog_only", "official_dla_catalog_or_authorized_copy", "https://quicksearch.dla.mil/qsDocDetails.aspx?ident_number=2640", true},
        {"mil-std-202-method-204:2024", "MIL-STD", "Electronic and Electrical Component Parts, Method 204: Vibration, High Frequency", "active", "High-frequency vibration of electronic/electrical components", "catalog_only", "official_dla_catalog_or_authorized_copy", "https://quicksearch.dla.mil/qsDocDetails.aspx?ident_number=280877", true},
        {"mil-std-461g", "MIL-STD", "Requirements for Control of Electromagnetic Interference Characteristics", "active", "EMI emissions and susceptibility verification", "catalog_only", "official_dla_catalog_or_authorized_copy", "https://quicksearch.dla.mil/WMX/Default.aspx?token=5738281", true},
        {"gjb-150-series", "GJB", "军用装备实验室环境试验方法系列（候选族）", "controlled-source-candidate", "Chinese military equipment environmental tests; exact edition must be verified", "controlled_source_required", "official_military_standard_service_center", "https://std.samr.gov.cn/", true},
        {"nasa-gsfc-std-7000b:2021", "NASA", "General Environmental Verification Standard for GSFC Flight Programs and Projects", "active", "GSFC payloads, subsystems and components environmental verification", "catalog_only", "public_official", "https://standards.nasa.gov/standard/gsfc/gsfc-std-7000", true},
        {"iec-60068-2-6:2007", "IEC", "Environmental testing — Test Fc: Vibration (sinusoidal)", "current", "Sinusoidal environmental vibration", "catalog_only", "purchase_or_authorized_copy", "https://webstore.iec.ch/en/publication/544", true},
        {"iec-60068-2-27:2008", "IEC", "Environmental testing — Test Ea and guidance: Shock", "current", "Environmental shock", "catalog_only", "purchase_or_authorized_copy", "https://webstore.iec.ch/en/publication/514", true},
        {"iec-60068-2-64:2008+a1:2019", "IEC", "Environmental testing — Test Fh: Vibration, broadband random", "current", "Broadband random vibration", "catalog_only", "purchase_or_authorized_copy", "https://webstore.iec.ch/en/publication/65892", true},
        {"iec-60068-2-80:2005", "IEC", "Environmental testing — Test Fi: Vibration, mixed mode", "current", "Mixed-mode deterministic/random vibration of specimens", "catalog_only", "purchase_or_authorized_copy", "https://webstore.iec.ch/en/publication/561", true},
        {"iec-61373:2026", "IEC", "Railway applications — rolling stock equipment shock and vibration tests", "current", "Railway rolling stock shock/vibration", "catalog_only", "purchase_or_authorized_copy", "https://webstore.iec.ch/en/publication/68999", true},
        {"rtca-do-160g", "RTCA", "Environmental Conditions and Test Procedures for Airborne Equipment", "purchase-required", "Civil airborne equipment environmental and EMI tests", "purchase_required", "authorized_rtca_seller", "https://www.rtca.org/do-160/", true},
        {"iso-16750-3:2023", "ISO", "Road vehicles — environmental conditions and testing for electrical and electronic equipment — mechanical loads", "published", "Road vehicle electronic equipment mechanical loads", "catalog_only", "purchase_or_authorized_copy", "https://www.iso.org/standard/77579.html?browse=tc", true},
        {"iec-60034-14:2018", "IEC", "Rotating electrical machines — Part 14: mechanical vibration of certain machines", "current", "Factory acceptance vibration procedures and limits for specified electrical machines", "catalog_only", "purchase_or_authorized_copy", "https://webstore.iec.ch/en/publication/59889", true},
        {"api-610:2021", "API", "Centrifugal Pumps for Petroleum, Petrochemical and Natural Gas Industries, 12th Edition", "published", "Process centrifugal pumps in petroleum, petrochemical and gas services", "purchase_required", "authorized_api_purchase", "https://www.api.org/-/media/files/publications/2025-catalog/06_refining_2025.pdf", true},
        {"api-617:2022", "API", "Axial and Centrifugal Compressors and Expander-Compressors, 9th Edition", "published", "Special-purpose compressors and expander-compressors for process gas/air", "purchase_required", "authorized_api_purchase", "https://www.api.org/-/media/files/publications/2025-catalog/06_refining_2025.pdf", true},
        {"api-618:2024", "API", "Reciprocating Compressors for Petroleum, Chemical and Gas Industry Services, 6th Edition", "published", "Low- to moderate-speed reciprocating compressors and drivers", "purchase_required", "authorized_api_purchase", "https://www.api.org/-/media/files/publications/2025-catalog/06_refining_2025.pdf", true},
        {"api-670:2014", "API", "Machinery Protection Systems, 5th Edition", "published-reaffirmed-2022", "Machinery protection systems measuring shaft/casing vibration, axial position and temperatures", "purchase_required", "authorized_api_purchase", "https://www.api.org/-/media/files/publications/2024-catalog/2024-refining.pdf", true},
        {"sae-j1455:2017", "SAE", "Recommended Environmental Practices for Electronic Equipment Design in Heavy-Duty Vehicle Applications", "published", "Heavy-duty vehicle electronic equipment environmental design/test practices", "purchase_required", "authorized_sae_purchase", "https://saemobilus.sae.org/standards/j1455_201703-recommended-environmental-practices-electronic-equipment-design-heavy-duty-vehicle-applications", true},
        {"iso-iec-17025", "ISO/IEC", "General requirements for the competence of testing and calibration laboratories", "published", "Laboratory competence and valid results", "catalog_only", "purchase_or_authorized_copy", "https://www.iso.org/ISO-IEC-17025-testing-and-calibration-laboratories.html", true},
        {"jcgm-100:2008", "JCGM/BIPM", "Guide to the expression of uncertainty in measurement (GUM)", "published-with-current-supplements", "Measurement uncertainty evaluation and reporting", "catalog_only", "public_official", "https://www.bipm.org/en/web/guest/publications/guides", true}
    };
    return entries;
}

bool isKnownStandardProfile(const std::string& id) {
    for (const auto& entry : standardCatalog())
        if (entry.id == id) return true;
    return false;
}

bool isExecutionApprovedStandardProfile(const std::string& id) {
    for (const auto& entry : standardCatalog())
        if (entry.id == id) return entry.executionStatus == "execution-approved";
    return false;
}

std::string standardsCatalogJson() {
    QJsonArray records;
    for (const auto& entry : standardCatalog()) {
        records.append(QJsonObject{
            {"id", QString::fromStdString(entry.id)},
            {"authority", QString::fromStdString(entry.authority)},
            {"title", QString::fromStdString(entry.title)},
            {"status", QString::fromStdString(entry.status)},
            {"applicability", QString::fromStdString(entry.applicability)},
            {"execution_status", QString::fromStdString(entry.executionStatus)},
            {"access", QString::fromStdString(entry.access)},
            {"source_url", QString::fromStdString(entry.sourceUrl)},
            {"requires_owner_approval", entry.requiresOwnerApproval}
        });
    }
    return AgentJson::dump(QJsonObject{
        {"schema", "dpa.standards.catalog"},
        {"schema_version", 1},
        {"license_policy", "metadata_and_derived_rules_only"},
        {"execution_default", "catalog_only"},
        {"records", records}
    });
}

std::vector<StandardCatalogEntry> searchStandardCatalog(const std::string& query,
                                                        const std::string& authority,
                                                        const std::string& executionStatus) {
    std::vector<StandardCatalogEntry> matches;
    for (const auto& entry : standardCatalog()) {
        if (!containsInsensitive(entry.id, query)
            && !containsInsensitive(entry.title, query)
            && !containsInsensitive(entry.applicability, query)) continue;
        if (!authority.empty() && !containsInsensitive(entry.authority, authority)) continue;
        if (!executionStatus.empty() && entry.executionStatus != executionStatus) continue;
        matches.push_back(entry);
    }
    return matches;
}

StandardApplicabilityResult assessStandardApplicability(const StandardApplicabilityRequest& request) {
    StandardApplicabilityResult result;
    result.standardProfile = request.standardProfile;

    if (request.standardProfile.empty() || request.standardProfile == "none") {
        result.disposition = "context_missing";
        addFinding(result, "standard_profile_required", "blocker",
                   "选择一个标准 profile，或明确记录本次试验不采用标准判定。");
        requireEvidence(result, "standard_selection_decision");
        return result;
    }

    const auto it = std::find_if(standardCatalog().begin(), standardCatalog().end(),
                                 [&](const StandardCatalogEntry& entry) {
                                     return entry.id == request.standardProfile;
                                 });
    if (it == standardCatalog().end()) {
        result.disposition = "unknown";
        addFinding(result, "unknown_standard_profile", "blocker",
                   "标准 profile 不在项目治理目录中，不能用于试验计划或判定。");
        requireEvidence(result, "official_catalog_record");
        return result;
    }
    result.known = true;

    const std::string id = it->id;
    const std::string machine = lowerAscii(request.machineType);
    const std::string platform = lowerAscii(request.platform);
    const std::string purpose = lowerAscii(request.testPurpose);

    if (request.machineType.empty()) {
        addFinding(result, "machine_type_required", "blocker", "必须由领域负责人提供被测设备类型。");
        requireEvidence(result, "asset_profile");
    }

    bool contextFailure = false;
    auto requirePlatform = [&](const char* expected, const char* code, const char* message) {
        if (platform.empty()) {
            addFinding(result, "platform_required", "blocker", "必须提供设备平台/任务环境。");
            requireEvidence(result, "platform_and_mission_profile");
            contextFailure = true;
        } else if (!containsInsensitive(platform, expected)) {
            addFinding(result, code, "blocker", message);
            contextFailure = true;
        }
    };
    auto requirePurpose = [&](const char* code, const char* message) {
        if (purpose.empty()) {
            addFinding(result, "test_purpose_required", "blocker", "必须提供本次试验目的。");
            requireEvidence(result, "test_objective");
            contextFailure = true;
        } else if (!containsInsensitive(purpose, code)) {
            // The caller may use Chinese or a domain-specific phrase. A broad
            // purpose is accepted here; the detailed method/level remains a
            // human-owned plan field.
            (void)message;
        }
    };

    if (id == "iso-20816-3:2022" || id == "gb-t-6075.3:2011") {
        if (!(request.ratedPowerKw > 15.0)) {
            addFinding(result, "power_range_not_met", "blocker",
                       "该 profile 的机器功率范围要求大于 15 kW。");
            requireEvidence(result, "rated_power_or_domain_basis");
            contextFailure = true;
        }
        if (!(request.rpm >= 120.0 && request.rpm <= 30000.0)) {
            addFinding(result, "speed_range_not_met", "blocker",
                       "该 profile 需要 120 至 30000 r/min 范围内的转速依据。");
            requireEvidence(result, "tachometer_or_speed_source");
            contextFailure = true;
        }
    } else if (id == "gb-t-6075.6:2024") {
        if (!(request.ratedPowerKw > 100.0)) {
            addFinding(result, "power_range_not_met", "blocker",
                       "GB/T 6075.6 profile 需要功率大于 100 kW。");
            requireEvidence(result, "rated_power_or_domain_basis");
            contextFailure = true;
        }
        if (!containsInsensitive(machine, "recip") && !containsInsensitive(machine, "往复")) {
            addFinding(result, "machine_family_not_met", "blocker",
                       "该 profile 只适用于往复式机器，不能用于未知设备类型。");
            requireEvidence(result, "machine_family_confirmation");
            contextFailure = true;
        }
    } else if (id == "iso-20816-1:2016" || id == "gb-t-6075.1:2012"
               || id == "gb-t-19873-series" || id == "iso-13373-2:2016"
               || id == "iso-13373-3:2015" || id == "iso-17359:2018") {
        // General monitoring/diagnostic profiles still require a named asset;
        // they do not authorize a severity threshold by themselves.
        if (request.physicalQuantity.empty() && id.find("13373") == std::string::npos
            && id.find("17359") == std::string::npos) {
            addFinding(result, "physical_quantity_required", "blocker",
                       "必须明确加速度、速度或位移测量量。");
            requireEvidence(result, "measurement_profile");
            contextFailure = true;
        }
    } else if (id == "gb-t-6075.4:2015") {
        if (!containsInsensitive(machine, "turbine") && !containsInsensitive(machine, "燃气")
            && !containsInsensitive(machine, "gas")) {
            addFinding(result, "machine_family_not_gas_turbine", "blocker",
                       "GB/T 6075.4 profile 需要具有滑动轴承的燃气轮机组。");
            requireEvidence(result, "machine_family_confirmation");
            contextFailure = true;
        }
        if (!containsInsensitive(purpose, "operational") && !containsInsensitive(purpose, "monitor")
            && !containsInsensitive(purpose, "运行") && !containsInsensitive(purpose, "监测")) {
            addFinding(result, "purpose_not_operational", "blocker",
                       "该机器族 profile 需要运行监测或评价目的。");
            requireEvidence(result, "test_objective");
            contextFailure = true;
        }
    } else if (id == "gb-t-6075.7:2015") {
        if (!containsInsensitive(machine, "pump") && !containsInsensitive(machine, "泵")) {
            addFinding(result, "machine_family_not_rotodynamic_pump", "blocker",
                       "GB/T 6075.7 profile 需要工业应用旋转动力泵。");
            requireEvidence(result, "machine_family_confirmation");
            contextFailure = true;
        }
        if (!containsInsensitive(purpose, "operational") && !containsInsensitive(purpose, "monitor")
            && !containsInsensitive(purpose, "运行") && !containsInsensitive(purpose, "监测")) {
            addFinding(result, "purpose_not_operational", "blocker",
                       "该机器族 profile 需要运行监测或评价目的。");
            requireEvidence(result, "test_objective");
            contextFailure = true;
        }
    } else if (id == "gb-t-9239.11:2025" || id == "gb-t-9239.21:2019"
               || id == "iso-21940-11:2016" || id == "iso-21940-12:2016"
               || id == "iso-21940-13:2012" || id == "iso-21940-21:2022") {
        requirePurpose("balanc", "");
        if (!containsInsensitive(purpose, "balanc") && !containsInsensitive(purpose, "平衡")) {
            addFinding(result, "purpose_not_balancing", "blocker",
                       "转子平衡 profile 需要明确的平衡试验目的。");
            requireEvidence(result, "balancing_objective_and_safety_plan");
            contextFailure = true;
        }
    } else if (id == "mil-std-167-1a:2022" || id == "di-envr-81647"
               || id == "mil-dtl-901e:2023") {
        requirePlatform("ship", "platform_not_shipboard",
                        "该 profile 只适用于舰船/舰载设备场景。");
        requirePurpose("", "");
    } else if (id == "rtca-do-160g") {
        requirePlatform("air", "platform_not_airborne",
                        "RTCA DO-160 profile 只适用于民用航空机载设备场景。");
        requirePurpose("", "");
    } else if (id == "iec-61373:2026") {
        requirePlatform("rail", "platform_not_railway",
                        "IEC 61373 profile 只适用于铁路车辆设备场景。");
        requirePurpose("", "");
    } else if (id == "iso-16750-3:2023") {
        requirePlatform("road", "platform_not_road_vehicle",
                        "ISO 16750-3 profile 只适用于道路车辆设备场景。");
        requirePurpose("", "");
    } else if (id == "sae-j1455:2017") {
        if (platform.empty()) {
            addFinding(result, "platform_required", "blocker", "SAE J1455 需要重型车辆平台和任务环境。");
            requireEvidence(result, "platform_and_mission_profile");
            contextFailure = true;
        } else if (!containsInsensitive(platform, "road") && !containsInsensitive(platform, "heavy")) {
            addFinding(result, "platform_not_heavy_vehicle", "blocker",
                       "SAE J1455 profile 只适用于重型车辆电子设备场景。");
            contextFailure = true;
        }
        requirePurpose("", "");
    } else if (id == "nasa-gsfc-std-7000b:2021") {
        requirePlatform("space", "platform_not_space",
                        "NASA GSFC profile 只适用于航天载荷/分系统场景。");
        requirePurpose("", "");
    } else if (id == "mil-std-810:2022" || id == "gjb-150-series"
               || id == "gb-t-2423.5:2019" || id == "gb-t-2423.10:2019"
               || id == "gb-t-2423.56:2023" || id == "gb-t-2423.62:2018"
               || id == "iec-60068-2-6:2007" || id == "iec-60068-2-27:2008"
               || id == "iec-60068-2-64:2008+a1:2019" || id == "iec-60068-2-80:2005"
               || id == "mil-std-202-method-204:2024") {
        requirePurpose("", "");
        requireEvidence(result, "tailoring_and_test_level_basis");
        if (request.applicabilityBasis.empty()) {
            addFinding(result, "tailoring_basis_required", "blocker",
                       "环境试验必须记录任务环境剪裁和试验等级依据。");
            contextFailure = true;
        }
    } else if (id == "iso-16063-13:2001" || id == "iso-16063-17:2016"
               || id == "iso-16063-21:2003" || id == "iso-16063-32:2016") {
        if (!containsInsensitive(purpose, "calibr") && !containsInsensitive(purpose, "校准")
            && !containsInsensitive(purpose, "uncert") && !containsInsensitive(purpose, "不确定")) {
            addFinding(result, "calibration_purpose_required", "blocker",
                       "ISO 16063 profile 需要明确传感器校准或不确定度用途。");
            requireEvidence(result, "calibration_scope_and_traceability");
            contextFailure = true;
        }
        requireEvidence(result, "calibration_scope_and_traceability");
    } else if (id == "iec-60034-14:2018") {
        if (!containsInsensitive(machine, "motor") && !containsInsensitive(machine, "electr")
            && !containsInsensitive(machine, "电机")) {
            addFinding(result, "machine_family_not_electrical", "blocker",
                       "IEC 60034-14 profile 需要旋转电机设备类型。");
            requireEvidence(result, "machine_family_confirmation");
            contextFailure = true;
        }
        requirePurpose("", "");
        requireEvidence(result, "factory_acceptance_condition");
    } else if (id == "api-610:2021" || id == "api-617:2022"
               || id == "api-618:2024" || id == "api-670:2014") {
        if (platform.empty()) {
            addFinding(result, "platform_required", "blocker",
                       "API 设备标准需要石油、化工、天然气或明确的过程工业平台。");
            requireEvidence(result, "industry_service_profile");
            contextFailure = true;
        } else if (!containsInsensitive(platform, "process") && !containsInsensitive(platform, "oil")
                   && !containsInsensitive(platform, "gas") && !containsInsensitive(platform, "petro")
                   && !containsInsensitive(platform, "chemical")) {
            addFinding(result, "platform_not_api_service", "blocker",
                       "当前平台不在 API 过程工业服务范围内。");
            contextFailure = true;
        }
        requirePurpose("", "");
        requireEvidence(result, "api_equipment_data_sheet_and_acceptance_basis");
    } else if (id == "iso-iec-17025" || id == "jcgm-100:2008") {
        if (!containsInsensitive(purpose, "govern") && !containsInsensitive(purpose, "uncert")
            && !containsInsensitive(purpose, "校准") && !containsInsensitive(purpose, "不确定")) {
            addFinding(result, "governance_purpose_required", "blocker",
                       "该条目属于实验室治理/不确定度用途，不是设备振动阈值 profile。");
            requireEvidence(result, "laboratory_governance_scope");
            contextFailure = true;
        }
    }

    if (it->executionStatus == "controlled_source_required") {
        requireEvidence(result, "authorized_controlled_copy");
        addFinding(result, "controlled_source_required", "evidence",
                   "该标准必须通过官方受控渠道取得，目录记录不能替代授权全文。");
    } else if (it->access.find("purchase") != std::string::npos
               || it->access.find("authorized") != std::string::npos) {
        requireEvidence(result, "authorized_copy_and_sha256");
        if (!request.authorizedCopy)
            addFinding(result, "authorized_copy_required", "evidence",
                       "执行前必须登记授权副本位置和 SHA-256。");
    }
    if (it->requiresOwnerApproval) {
        requireEvidence(result, "domain_owner_approval");
        if (!request.ownerApproved)
            addFinding(result, "domain_owner_approval_required", "evidence",
                       "执行前必须由领域负责人批准适用性和判定目的。");
    }
    // A boolean such as authorizedCopy is not an auditable evidence chain. The
    // service must first validate a manifest containing the local copy hash and
    // clause-level locators that cover this profile.
    requireEvidence(result, "authorized_clause_evidence_manifest");
    if (!request.authorizedEvidenceValidated) {
        addFinding(result, "authorized_clause_evidence_required", "evidence",
                   "执行前必须登记覆盖该 profile 的授权副本哈希和条款级引用。");
    } else if (request.authorizedEvidenceManifestSha256.size() != 64) {
        addFinding(result, "authorized_clause_evidence_digest_invalid", "blocker",
                   "授权证据清单摘要哈希无效，不能建立执行链。"
                   );
    }
    if (request.applicabilityBasis.empty()
        && id != "iso-13373-2:2016" && id != "iso-13373-3:2015"
        && id != "iso-17359:2018") {
        requireEvidence(result, "applicability_basis");
    }

    const bool hasBlocker = std::any_of(result.findings.begin(), result.findings.end(),
                                        [](const StandardApplicabilityFinding& finding) {
                                            return finding.severity == "blocker";
                                        });
    result.applicable = !hasBlocker && !contextFailure;
    result.executionReady = result.applicable && request.authorizedCopy
                            && request.ownerApproved
                            && request.authorizedEvidenceValidated
                            && it->executionStatus == "execution-approved";
    if (hasBlocker || contextFailure) result.disposition = "context_missing";
    else if (!result.executionReady) result.disposition = "review_required";
    else result.disposition = "execution_ready";
    return result;
}

StandardTestPlanResult assessStandardTestPlan(const StandardTestPlanRequest& request) {
    StandardTestPlanResult result;
    std::set<std::string> seenProfiles;
    for (const auto& id : request.requestedStandardProfiles) {
        if (!seenProfiles.insert(id).second) {
            result.blockers.push_back("duplicate_standard_profile:" + id);
            continue;
        }
        StandardApplicabilityRequest assessmentRequest;
        assessmentRequest.standardProfile = id;
        assessmentRequest.machineType = request.machineType;
        assessmentRequest.ratedPowerKw = request.ratedPowerKw;
        assessmentRequest.rpm = request.rpm;
        assessmentRequest.platform = request.platform;
        assessmentRequest.testPurpose = request.testPurpose;
        assessmentRequest.physicalQuantity = request.physicalQuantity;
        assessmentRequest.applicabilityBasis = request.applicabilityBasis;
        assessmentRequest.authorizedCopy = request.authorizedCopy;
        assessmentRequest.ownerApproved = request.ownerApproved;
        assessmentRequest.authorizedEvidenceValidated = request.authorizedEvidenceValidated;
        assessmentRequest.authorizedEvidenceManifestSha256 = request.authorizedEvidenceManifestSha256;
        const StandardApplicabilityResult assessment =
            assessStandardApplicability(assessmentRequest);
        result.assessments.push_back(assessment);
        if (assessment.disposition == "unknown" || assessment.disposition == "context_missing"
            || assessment.disposition == "not_applicable" || assessment.disposition == "review_required") {
            result.blockers.push_back(id + ":" + assessment.disposition);
        }
        for (const auto& finding : assessment.findings) {
            if (finding.severity == "warning") result.warnings.push_back(id + ":" + finding.code);
        }
        for (const auto& evidence : assessment.requiredEvidence)
            if (std::find(result.requiredEvidence.begin(), result.requiredEvidence.end(), evidence)
                == result.requiredEvidence.end()) result.requiredEvidence.push_back(evidence);
    }
    if (result.assessments.empty()) result.blockers.push_back("standard_profiles_required");
    // A plan is executable only when every selected profile is execution-ready.
    result.ready = !result.assessments.empty() && result.blockers.empty()
                   && std::all_of(result.assessments.begin(), result.assessments.end(),
                                  [](const StandardApplicabilityResult& assessment) {
                                      return assessment.executionReady;
                                  });
    return result;
}

static QJsonObject applicabilityObject(const StandardApplicabilityResult& result) {
    QJsonArray findings;
    for (const auto& finding : result.findings) {
        findings.append(QJsonObject{{"code", QString::fromStdString(finding.code)},
                                    {"severity", QString::fromStdString(finding.severity)},
                                    {"message", QString::fromStdString(finding.message)}});
    }
    QJsonArray evidence;
    for (const auto& item : result.requiredEvidence)
        evidence.append(QString::fromStdString(item));
    return QJsonObject{{"standard_profile", QString::fromStdString(result.standardProfile)},
                       {"disposition", QString::fromStdString(result.disposition)},
                       {"known", result.known},
                       {"applicable", result.applicable},
                       {"execution_ready", result.executionReady},
                       {"findings", findings},
                       {"required_evidence", evidence}};
}

std::string standardApplicabilityJson(const StandardApplicabilityResult& result) {
    return AgentJson::dump(applicabilityObject(result));
}

std::string standardTestPlanJson(const StandardTestPlanResult& result) {
    QJsonArray assessments;
    for (const auto& assessment : result.assessments)
        assessments.append(applicabilityObject(assessment));
    QJsonArray blockers;
    for (const auto& item : result.blockers) blockers.append(QString::fromStdString(item));
    QJsonArray warnings;
    for (const auto& item : result.warnings) warnings.append(QString::fromStdString(item));
    QJsonArray evidence;
    for (const auto& item : result.requiredEvidence) evidence.append(QString::fromStdString(item));
    return AgentJson::dump(QJsonObject{{"ready", result.ready},
                                       {"assessments", assessments},
                                       {"blockers", blockers},
                                       {"warnings", warnings},
                                       {"required_evidence", evidence}});
}

namespace {

constexpr int kStandardPlanSchemaVersion = 1;
constexpr int kMaxStandardPlanBytes = 128 * 1024;

QByteArray compactObject(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QString sha256Object(const QJsonObject& object) {
    return QCryptographicHash::hash(compactObject(object), QCryptographicHash::Sha256).toHex();
}

bool validSha256(const QJsonValue& value, bool allowEmpty = false) {
    if (!value.isString()) return false;
    const QString text = value.toString();
    if (allowEmpty && text.isEmpty()) return true;
    if (text.size() != 64) return false;
    for (const QChar ch : text) {
        const ushort code = ch.unicode();
        if (!((code >= '0' && code <= '9') || (code >= 'a' && code <= 'f'))) return false;
    }
    return true;
}

bool exactKeys(const QJsonObject& object, const QStringList& keys) {
    if (object.size() != keys.size()) return false;
    for (const QString& key : keys)
        if (!object.contains(key)) return false;
    return true;
}

bool finiteInRange(const QJsonValue& value, double minimum, double maximum) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    return std::isfinite(number) && number >= minimum && number <= maximum;
}

QJsonObject planContextObject(const StandardTestPlanRequest& request) {
    return QJsonObject{
        {"machine_type", QString::fromStdString(request.machineType)},
        {"rated_power_kw", request.ratedPowerKw},
        {"rpm", request.rpm},
        {"platform", QString::fromStdString(request.platform)},
        {"test_purpose", QString::fromStdString(request.testPurpose)},
        {"physical_quantity", QString::fromStdString(request.physicalQuantity)},
        {"applicability_basis", QString::fromStdString(request.applicabilityBasis)},
        {"authorized_copy", request.authorizedCopy},
        {"owner_approved", request.ownerApproved},
        {"authorized_evidence_validated", request.authorizedEvidenceValidated},
        {"authorized_evidence_manifest_sha256",
         QString::fromStdString(request.authorizedEvidenceManifestSha256)},
        {"experiment_context_sha256", QString::fromStdString(request.experimentContextSha256)}
    };
}

QJsonObject planBody(const StandardTestPlanRequest& request,
                    const StandardTestPlanResult& result) {
    QJsonArray requested;
    for (const auto& id : request.requestedStandardProfiles)
        requested.append(QString::fromStdString(id));
    QJsonArray assessments;
    for (const auto& assessment : result.assessments)
        assessments.append(applicabilityObject(assessment));
    QJsonArray blockers;
    for (const auto& item : result.blockers) blockers.append(QString::fromStdString(item));
    QJsonArray warnings;
    for (const auto& item : result.warnings) warnings.append(QString::fromStdString(item));
    QJsonArray evidence;
    for (const auto& item : result.requiredEvidence)
        evidence.append(QString::fromStdString(item));
    return QJsonObject{
        {"schema", "dpa.standards.test-plan"},
        {"schema_version", kStandardPlanSchemaVersion},
        {"context", planContextObject(request)},
        {"requested_standard_profiles", requested},
        {"assessments", assessments},
        {"ready", result.ready},
        {"blockers", blockers},
        {"warnings", warnings},
        {"required_evidence", evidence}
    };
}

bool boundedString(const QJsonValue& value, int maximum, bool allowEmpty = true) {
    return value.isString() && (allowEmpty || !value.toString().isEmpty())
        && value.toString().toUtf8().size() <= maximum;
}

} // namespace

std::string buildStandardTestPlanJson(const StandardTestPlanRequest& request) {
    if (!std::isfinite(request.ratedPowerKw) || request.ratedPowerKw < 0
        || !std::isfinite(request.rpm) || request.rpm < 0
        || request.machineType.size() > 256 || request.platform.size() > 128
        || request.testPurpose.size() > 256 || request.physicalQuantity.size() > 128
        || request.applicabilityBasis.size() > 2048
        || request.experimentContextSha256.size() > 64
        || request.authorizedEvidenceManifestSha256.size() > 64
        || (request.authorizedEvidenceValidated
            && !validSha256(QJsonValue(QString::fromStdString(
                    request.authorizedEvidenceManifestSha256))))) return {};
    const StandardTestPlanResult result = assessStandardTestPlan(request);
    QJsonObject body = planBody(request, result);
    body.insert("plan_sha256", sha256Object(body));
    return AgentJson::dump(body);
}

StandardTestPlanValidation validateStandardTestPlanJson(
    const std::string& json, const std::string& expectedExperimentContextSha256) {
    StandardTestPlanValidation result;
    auto fail = [&result](const char* error) {
        result.error = error;
        return result;
    };
    if (json.empty() || json.size() > static_cast<size_t>(kMaxStandardPlanBytes))
        return fail("standard_plan_size_invalid");
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(json),
                                                             &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail("standard_plan_json_invalid");
    const QJsonObject root = document.object();
    if (!exactKeys(root, {"schema", "schema_version", "context", "requested_standard_profiles",
                          "assessments", "ready", "blockers", "warnings", "required_evidence",
                          "plan_sha256"})
        || root.value("schema").toString() != "dpa.standards.test-plan"
        || !root.value("schema_version").isDouble()
        || root.value("schema_version").toInt() != kStandardPlanSchemaVersion
        || !root.value("context").isObject()
        || !root.value("requested_standard_profiles").isArray()
        || !root.value("assessments").isArray()
        || !root.value("ready").isBool()
        || !root.value("blockers").isArray()
        || !root.value("warnings").isArray()
        || !root.value("required_evidence").isArray()
        || !validSha256(root.value("plan_sha256")))
        return fail("standard_plan_schema_invalid");

    const QJsonObject context = root.value("context").toObject();
    if (!exactKeys(context, {"machine_type", "rated_power_kw", "rpm", "platform", "test_purpose",
                             "physical_quantity", "applicability_basis", "authorized_copy",
                             "owner_approved", "authorized_evidence_validated",
                             "authorized_evidence_manifest_sha256", "experiment_context_sha256"})
        || !boundedString(context.value("machine_type"), 256, false)
        || !finiteInRange(context.value("rated_power_kw"), 0, 1e9)
        || !finiteInRange(context.value("rpm"), 0, 1e7)
        || !boundedString(context.value("platform"), 128)
        || !boundedString(context.value("test_purpose"), 256)
        || !boundedString(context.value("physical_quantity"), 128)
        || !boundedString(context.value("applicability_basis"), 2048)
        || !context.value("authorized_copy").isBool()
        || !context.value("owner_approved").isBool()
        || !context.value("authorized_evidence_validated").isBool()
        || !validSha256(context.value("authorized_evidence_manifest_sha256"), true)
        || !validSha256(context.value("experiment_context_sha256"), true))
        return fail("standard_plan_context_invalid");

    const std::string contextSha = context.value("experiment_context_sha256")
        .toString().toStdString();
    if (!expectedExperimentContextSha256.empty()
        && contextSha != expectedExperimentContextSha256)
        return fail("standard_plan_context_mismatch");
    if (!expectedExperimentContextSha256.empty() && !validSha256(
            QJsonValue(QString::fromStdString(expectedExperimentContextSha256))))
        return fail("standard_plan_expected_context_invalid");

    const QJsonArray requested = root.value("requested_standard_profiles").toArray();
    if (requested.isEmpty() || requested.size() > 64)
        return fail("standard_plan_profiles_invalid");
    StandardTestPlanRequest request;
    request.machineType = context.value("machine_type").toString().toStdString();
    request.ratedPowerKw = context.value("rated_power_kw").toDouble();
    request.rpm = context.value("rpm").toDouble();
    request.platform = context.value("platform").toString().toStdString();
    request.testPurpose = context.value("test_purpose").toString().toStdString();
    request.physicalQuantity = context.value("physical_quantity").toString().toStdString();
    request.applicabilityBasis = context.value("applicability_basis").toString().toStdString();
    request.authorizedCopy = context.value("authorized_copy").toBool();
    request.ownerApproved = context.value("owner_approved").toBool();
    request.authorizedEvidenceValidated = context.value("authorized_evidence_validated").toBool();
    request.authorizedEvidenceManifestSha256 = context.value("authorized_evidence_manifest_sha256")
        .toString().toStdString();
    request.experimentContextSha256 = contextSha;
    std::set<std::string> unique;
    for (const QJsonValue& value : requested) {
        if (!boundedString(value, 160, false)) return fail("standard_plan_profile_invalid");
        const std::string id = value.toString().toStdString();
        if (!unique.insert(id).second) return fail("standard_plan_duplicate_profile");
        request.requestedStandardProfiles.push_back(id);
    }

    const StandardTestPlanResult expected = assessStandardTestPlan(request);
    QJsonObject expectedBody = planBody(request, expected);
    const QString digest = sha256Object(expectedBody);
    if (digest != root.value("plan_sha256").toString())
        return fail("standard_plan_digest_mismatch");
    QJsonObject suppliedBody = root;
    suppliedBody.remove("plan_sha256");
    if (suppliedBody != expectedBody)
        return fail("standard_plan_recomputed_mismatch");

    result.ok = true;
    result.error.clear();
    result.planSha256 = digest.toStdString();
    result.ready = expected.ready;
    result.profileCount = static_cast<int>(expected.assessments.size());
    return result;
}

} // namespace Model
