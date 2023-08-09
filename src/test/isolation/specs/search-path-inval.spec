# Test search_path invalidation.

setup
{
    CREATE USER regress_sp_user1;
    CREATE SCHEMA regress_sp_user1 AUTHORIZATION regress_sp_user1;
    CREATE SCHEMA regress_sp_public;
    GRANT ALL PRIVILEGES ON SCHEMA regress_sp_public TO regress_sp_user1;
    CREATE TABLE regress_sp_user1.x(t) AS SELECT 'data in regress_sp_user1.x';
    GRANT SELECT ON regress_sp_user1.x TO regress_sp_user1;
    CREATE TABLE regress_sp_public.x(t) AS SELECT 'data in regress_sp_public.x';
    GRANT SELECT ON regress_sp_public.x TO regress_sp_user1;
}

teardown
{
    DROP SCHEMA regress_sp_public CASCADE;
    DROP SCHEMA regress_sp_user1 CASCADE;
    DROP USER regress_sp_user1;
}

session s1
setup
{
    SET search_path = "$user", regress_sp_public;
    SET SESSION AUTHORIZATION regress_sp_user1;
}
step s1a
{
    SELECT CURRENT_USER;
    SELECT t FROM x;
}

session s2
step s2a
{
    ALTER ROLE regress_sp_user1 RENAME TO regress_sp_user2;
}
step s2b
{
    ALTER ROLE regress_sp_user2 RENAME TO regress_sp_user1;
}
step s2c
{
    ALTER SCHEMA regress_sp_user1 RENAME TO regress_sp_user2;
}
step s2d
{
    ALTER SCHEMA regress_sp_user2 RENAME TO regress_sp_user1;
}

# Run all permutations. When s1a falls between s2a and s2b, or between
# s2c and s2d, the role name does not match the schema name. In those
# cases, search_path should be invalidated and fall back to
# regress_sp_public.x.
