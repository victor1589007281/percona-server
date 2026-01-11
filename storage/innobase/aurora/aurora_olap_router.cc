/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora OLAP Query Router Implementation

*****************************************************************************/

#include "aurora_olap_router.h"
#include <algorithm>
#include <cctype>

namespace aurora {

// Global instance
std::unique_ptr<OLAPQueryRouter> g_olap_router;

//============================================================================
// OLAPQueryRouter Implementation
//============================================================================

OLAPQueryRouter::OLAPQueryRouter()
    : auto_olap_enabled_(false),
      olap_available_(false),
      row_threshold_(10000) {
  memset(&stats_, 0, sizeof(stats_));
}

OLAPQueryRouter::~OLAPQueryRouter() {}

void OLAPQueryRouter::initialize() {
  init_patterns();
  init_default_rules();
}

void OLAPQueryRouter::init_patterns() {
  // Case-insensitive patterns
  select_pattern_ = std::regex(R"(^\s*SELECT\s)", std::regex::icase);
  aggregation_pattern_ = std::regex(
      R"(\b(COUNT|SUM|AVG|MIN|MAX|GROUP_CONCAT)\s*\()", std::regex::icase);
  group_by_pattern_ = std::regex(R"(\bGROUP\s+BY\b)", std::regex::icase);
  order_by_pattern_ = std::regex(R"(\bORDER\s+BY\b)", std::regex::icase);
  join_pattern_ = std::regex(R"(\b(INNER|LEFT|RIGHT|OUTER|CROSS)?\s*JOIN\b)", std::regex::icase);
  where_eq_pattern_ = std::regex(R"(\bWHERE\s+\w+\s*=\s*)", std::regex::icase);
  
  // Hint patterns
  hint_olap_pattern_ = std::regex(R"(/\*\+\s*USE_OLAP\s*\*/)", std::regex::icase);
  hint_innodb_pattern_ = std::regex(R"(/\*\+\s*USE_INNODB\s*\*/)", std::regex::icase);
}

void OLAPQueryRouter::init_default_rules() {
  // Rule 1: Aggregation queries -> OLAP
  rules_.push_back({
    "aggregation_query",
    std::regex(R"(SELECT.*\b(COUNT|SUM|AVG)\s*\()", std::regex::icase),
    QueryType::OLAP,
    RoutingDecision::USE_OLAP,
    0,
    100,
    true
  });
  
  // Rule 2: GROUP BY queries -> OLAP
  rules_.push_back({
    "group_by_query",
    std::regex(R"(\bGROUP\s+BY\b)", std::regex::icase),
    QueryType::OLAP,
    RoutingDecision::USE_OLAP,
    0,
    90,
    true
  });
  
  // Rule 3: Point lookups -> InnoDB
  rules_.push_back({
    "point_lookup",
    std::regex(R"(WHERE\s+\w+\s*=\s*(\d+|'[^']*')\s*(AND\s+\w+\s*=\s*(\d+|'[^']*'))*\s*(LIMIT\s+1)?$)", std::regex::icase),
    QueryType::OLTP,
    RoutingDecision::USE_INNODB,
    0,
    80,
    true
  });
  
  // Rule 4: SELECT * without WHERE -> OLAP for large tables
  rules_.push_back({
    "full_table_scan",
    std::regex(R"(SELECT\s+\*\s+FROM\s+\w+\s*$)", std::regex::icase),
    QueryType::OLAP,
    RoutingDecision::USE_OLAP,
    10000,  // Only if > 10k rows
    70,
    true
  });
  
  // Sort rules by priority
  std::sort(rules_.begin(), rules_.end(),
            [](const RoutingRule& a, const RoutingRule& b) {
              return a.priority > b.priority;
            });
}

QueryAnalysis OLAPQueryRouter::analyze(const std::string& sql) {
  QueryAnalysis analysis;
  memset(&analysis, 0, sizeof(analysis));
  analysis.type = QueryType::OLTP;  // Default
  
  stats_.queries_analyzed++;
  
  // Check if SELECT
  if (!std::regex_search(sql, select_pattern_)) {
    // Not a SELECT query
    if (sql.find("CREATE") != std::string::npos ||
        sql.find("ALTER") != std::string::npos ||
        sql.find("DROP") != std::string::npos) {
      analysis.type = QueryType::DDL;
    }
    return analysis;
  }
  
  // Check for aggregation
  analysis.has_aggregation = std::regex_search(sql, aggregation_pattern_);
  
  // Check for GROUP BY
  analysis.has_group_by = std::regex_search(sql, group_by_pattern_);
  
  // Check for ORDER BY
  analysis.has_order_by = std::regex_search(sql, order_by_pattern_);
  
  // Check for JOIN
  analysis.has_join = std::regex_search(sql, join_pattern_);
  
  // Check for point lookup
  analysis.has_point_lookup = std::regex_search(sql, where_eq_pattern_) &&
                              sql.find("LIMIT 1") != std::string::npos;
  
  // Classify query type
  if (analysis.has_aggregation || analysis.has_group_by) {
    analysis.type = QueryType::OLAP;
  } else if (analysis.has_point_lookup) {
    analysis.type = QueryType::OLTP;
  } else if (analysis.has_join) {
    analysis.type = QueryType::HYBRID;
  }
  
  return analysis;
}

bool OLAPQueryRouter::match_hint(const std::string& sql, bool& use_olap) {
  // Check for /*+ USE_OLAP */ hint
  if (std::regex_search(sql, hint_olap_pattern_)) {
    use_olap = true;
    return true;
  }
  
  // Check for /*+ USE_INNODB */ hint
  if (std::regex_search(sql, hint_innodb_pattern_)) {
    use_olap = false;
    return true;
  }
  
  return false;
}

RoutingDecision OLAPQueryRouter::route(const std::string& sql, uint64_t estimated_rows) {
  // Check if OLAP is available
  if (!olap_available_) {
    return RoutingDecision::USE_INNODB;
  }
  
  // Check for explicit hints first
  bool use_olap = false;
  if (match_hint(sql, use_olap)) {
    if (use_olap) {
      stats_.queries_routed_olap++;
      return RoutingDecision::USE_OLAP;
    } else {
      stats_.queries_routed_innodb++;
      return RoutingDecision::USE_INNODB;
    }
  }
  
  // If auto OLAP is disabled, default to InnoDB
  if (!auto_olap_enabled_) {
    stats_.queries_routed_innodb++;
    return RoutingDecision::USE_INNODB;
  }
  
  // Check rules
  for (const auto& rule : rules_) {
    if (!rule.enabled) continue;
    
    if (std::regex_search(sql, rule.pattern)) {
      stats_.rule_matches++;
      
      // Check row threshold if specified
      if (rule.row_threshold > 0 && estimated_rows < (uint64_t)rule.row_threshold) {
        continue;  // Skip this rule, try next
      }
      
      if (rule.decision == RoutingDecision::USE_OLAP) {
        stats_.queries_routed_olap++;
      } else {
        stats_.queries_routed_innodb++;
      }
      
      return rule.decision;
    }
  }
  
  // Default: use row threshold
  if (estimated_rows >= row_threshold_) {
    stats_.queries_routed_olap++;
    return RoutingDecision::USE_OLAP;
  }
  
  stats_.queries_routed_innodb++;
  return RoutingDecision::USE_INNODB;
}

RoutingDecision OLAPQueryRouter::route(const QueryAnalysis& analysis) {
  if (!olap_available_) {
    return RoutingDecision::USE_INNODB;
  }
  
  if (!auto_olap_enabled_) {
    return RoutingDecision::USE_INNODB;
  }
  
  switch (analysis.type) {
    case QueryType::OLAP:
      stats_.queries_routed_olap++;
      return RoutingDecision::USE_OLAP;
    
    case QueryType::OLTP:
      stats_.queries_routed_innodb++;
      return RoutingDecision::USE_INNODB;
    
    case QueryType::HYBRID:
      // Use row estimate
      if (analysis.estimated_rows >= row_threshold_) {
        stats_.queries_routed_olap++;
        return RoutingDecision::USE_OLAP;
      }
      stats_.queries_routed_innodb++;
      return RoutingDecision::USE_INNODB;
    
    case QueryType::DDL:
    case QueryType::ADMIN:
    default:
      stats_.queries_routed_innodb++;
      return RoutingDecision::USE_INNODB;
  }
}

void OLAPQueryRouter::add_rule(const RoutingRule& rule) {
  rules_.push_back(rule);
  
  // Re-sort by priority
  std::sort(rules_.begin(), rules_.end(),
            [](const RoutingRule& a, const RoutingRule& b) {
              return a.priority > b.priority;
            });
}

void OLAPQueryRouter::remove_rule(const std::string& name) {
  rules_.erase(
      std::remove_if(rules_.begin(), rules_.end(),
                     [&name](const RoutingRule& r) { return r.name == name; }),
      rules_.end());
}

void OLAPQueryRouter::set_auto_olap(bool enabled) {
  auto_olap_enabled_ = enabled;
}

void OLAPQueryRouter::set_row_threshold(uint64_t threshold) {
  row_threshold_ = threshold;
}

OLAPQueryRouter::Stats OLAPQueryRouter::get_stats() const {
  return stats_;
}

//============================================================================
// Global Functions
//============================================================================

bool aurora_olap_router_init() {
  g_olap_router = std::make_unique<OLAPQueryRouter>();
  g_olap_router->initialize();
  return true;
}

void aurora_olap_router_shutdown() {
  g_olap_router.reset();
}

RoutingDecision aurora_route_query(const std::string& sql, uint64_t estimated_rows) {
  if (!g_olap_router) {
    return RoutingDecision::USE_INNODB;
  }
  return g_olap_router->route(sql, estimated_rows);
}

}  // namespace aurora
