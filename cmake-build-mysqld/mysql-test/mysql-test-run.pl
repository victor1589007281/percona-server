#!/usr/bin/perl
# Call mtr in out-of-source build
$ENV{MTR_BINDIR} = '/Users/victor/mysql/percona-server/cmake-build-mysqld';
chdir('/Users/victor/mysql/percona-server/mysql-test');
exit(system($^X, '/Users/victor/mysql/percona-server/mysql-test/mysql-test-run.pl', @ARGV) >> 8);
