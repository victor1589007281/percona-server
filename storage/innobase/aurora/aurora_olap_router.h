/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora OLAP Query Router
Routes queries between OLTP (InnoDB) and OLAP (Column Store) engines.

Reference: 01_compute_layer.md Section 14

*****************************************************************************/

#ifndef AURORA_OLAP_ROUTER_H
#define AURORA_OLAP_ROUTER_H

#include <cstdint>
#include <string>
#include <vector>
#include <regex>
#include <memory>

namespace aurora {

/**
 * Query Type Classification
 */
enum class QueryType {
  OLTP,           // Point queries, small range scans
  OLAP,           // Aggregation, full table scans
  HYBRID,         // Could go either way
  DDL,            // DDL operations (always OLTP)
  ADMIN           // Admin commands
};

/**
 * Routing Decision
 */
enum class RoutingDecision {
  USE_INNODB,     // Route to InnoDB (OLTP)
  USE_OLAP,       // Route to OLAP column store
  PARALLEL        // Execute on both and merge
};

/**
 * Query Analysis Result
 */
struct QueryAnalysis {
  QueryType type;
  bool has_aggregation;
  bool has_group_by;
  bool has_order_by;
  bool has_join;
  bool has_subquery;
  bool has_point_lookup;  // WHERE id = ?
  bool has_range_scan;
  uint64_t estimated_rows;
  std::vector<std::string> tables;
  std::vector<std::string> columns;
};

/**
 * Routing Rule
 */
struct RoutingRule {
  std::string name;
  std::regex pattern;         // SQL pattern to match
  QueryType query_type;
  RoutingDecision decision;
  int64_t row_threshold;      // Use OLAP if rows exceed this
  int priority;               // Higher priority rules checked first
  bool enabled;
};

/**
 * OLAP Query Router
 */
class OLAPQueryRouter {
public:
  OLAPQueryRouter();
  ~OLAPQueryRouter();

  /**
   * Initialize router with default rules
   */
  void initialize();

  /**
   * Analyze a SQL query
   */
  QueryAnalysis analyze(const std::string& sql);

  /**
   * Make routing decision for a query
   */
  RoutingDecision route(const std::string& sql, uint64_t estimated_rows = 0);

  /**
   * Make routing decision based on analysis
   */
  RoutingDecision route(const QueryAnalysis& analysis);

  /**
   * Add a routing rule
   */
  void add_rule(const RoutingRule& rule);

  /**
   * Remove a routing rule by name
   */
  void remove_rule(const std::string& name);

  /**
   * Enable/disable auto OLAP routing
   */
  void set_auto_olap(bool enabled);
  bool is_auto_olap() const { return auto_olap_enabled_; }

  /**
   * Set row threshold for OLAP routing
   */
  void set_row_threshold(uint64_t threshold);
  uint64_t get_row_threshold() const { return row_threshold_; }

  /**
   * Check if OLAP engine is available
   */
  bool is_olap_available() const { return olap_available_; }
  void set_olap_available(bool available) { olap_available_ = available; }

  /**
   * Get routing statistics
   */
  struct Stats {
    uint64_t queries_routed_innodb;
    uint64_t queries_routed_olap;
    uint64_t queries_analyzed;
    uint64_t rule_matches;
  };
  Stats get_stats() const;

private:
  bool auto_olap_enabled_;
  bool olap_available_;
  uint64_t row_threshold_;
  std::vector<RoutingRule> rules_;
  
  // Statistics
  mutable Stats stats_;
  
  // Regex patterns for query analysis
  std::regex select_pattern_;
  std::regex aggregation_pattern_;
  std::regex group_by_pattern_;
  std::regex order_by_pattern_;
  std::regex join_pattern_;
  std::regex where_eq_pattern_;
  std::regex hint_olap_pattern_;
  std::regex hint_innodb_pattern_;
  
  void init_default_rules();
  void init_patterns();
  bool match_hint(const std::string& sql, bool& use_olap);
};

/**
 * Global OLAP router instance
 */
extern std::unique_ptr<OLAPQueryRouter> g_olap_router;

/**
 * Initialize OLAP router
 */
bool aurora_olap_router_init();

/**
 * Shutdown OLAP router
 */
void aurora_olap_router_shutdown();

/**
 * Route a query (convenience function)
 */
RoutingDecision aurora_route_query(const std::string& sql, uint64_t estimated_rows = 0);

/**
 * Check if query should use OLAP
 */
inline bool aurora_should_use_olap(const std::string& sql, uint64_t estimated_rows = 0) {
  if (!g_olap_router || !g_olap_router->is_olap_available()) {
    return false;
  }
  return aurora_route_query(sql, estimated_rows) == RoutingDecision::USE_OLAP;
}

}  // namespace aurora

//============================================================================
// MySQL Integration Macros
//============================================================================

#ifdef HAVE_AURORA

/**
 * Check if query should be routed to OLAP
 * Use in sql/sql_parse.cc or sql/sql_select.cc
 */
#define AURORA_CHECK_OLAP_ROUTE(sql, rows) \
  aurora::aurora_should_use_olap(sql, rows)

/**
 * OLAP hint comment pattern
 * Usage in SQL: SELECT /*+ USE_OLAP */ ...
 */
#define AURORA_OLAP_HINT "USE_OLAP"
#define AURORA_INNODB_HINT "USE_INNODB"

#else

#define AURORA_CHECK_OLAP_ROUTE(sql, rows) (false)

#endif  // HAVE_AURORA

#endif  // AURORA_OLAP_ROUTER_H
