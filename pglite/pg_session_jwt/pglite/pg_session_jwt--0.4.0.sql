CREATE SCHEMA IF NOT EXISTS auth;

CREATE OR REPLACE FUNCTION auth."session"() RETURNS jsonb
STABLE PARALLEL SAFE
LANGUAGE plpgsql
AS $$
DECLARE
  claims_text text;
BEGIN
  claims_text := current_setting('request.jwt.claims', true);

  IF claims_text IS NULL OR claims_text = '' THEN
    RETURN NULL;
  END IF;

  BEGIN
    RETURN claims_text::jsonb;
  EXCEPTION
    WHEN others THEN
      RETURN NULL;
  END;
END;
$$;

CREATE OR REPLACE FUNCTION auth."jwt"() RETURNS jsonb
STABLE PARALLEL SAFE
LANGUAGE sql
AS $$
  SELECT auth."session"();
$$;

CREATE OR REPLACE FUNCTION auth."user_id"() RETURNS text
STABLE PARALLEL SAFE
LANGUAGE sql
AS $$
  SELECT auth."session"() ->> 'sub';
$$;

CREATE OR REPLACE FUNCTION auth."uid"() RETURNS uuid
STABLE PARALLEL SAFE
LANGUAGE plpgsql
AS $$
DECLARE
  sub text;
BEGIN
  sub := auth."user_id"();
  IF sub IS NULL OR sub = '' THEN
    RETURN NULL;
  END IF;

  BEGIN
    RETURN sub::uuid;
  EXCEPTION
    WHEN others THEN
      RETURN NULL;
  END;
END;
$$;

CREATE OR REPLACE FUNCTION auth."init"() RETURNS void
LANGUAGE plpgsql
AS $$
BEGIN
  RAISE EXCEPTION
    'pg_session_jwt JWK validation is not supported in this PGlite shim; set request.jwt.claims instead';
END;
$$;

CREATE OR REPLACE FUNCTION auth."jwt_session_init"(jwt text) RETURNS void
LANGUAGE plpgsql
AS $$
BEGIN
  RAISE EXCEPTION
    'pg_session_jwt JWK validation is not supported in this PGlite shim; set request.jwt.claims instead';
END;
$$;
