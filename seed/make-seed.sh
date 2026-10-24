#!/usr/bin/env bash
# Builds the seed database that every test and benchmark run copies.
#
#   bash seed/make-seed.sh          # full seed -> database conduit_seed (~2.4 GB, what the benchmark uses; ~5-10 min)
#   bash seed/make-seed.sh --dev    # 2% seed   -> database conduit_dev  (all 100k users and 200 tags, 10k articles;
#                                   #              what CI and test/run.sh use; ~30 s)
#
# Needs Go >= 1.27 (to build bench/cmd/seedgen) and psql. Connects as a Postgres superuser through the usual libpq
# variables (PGHOST, PGPORT, PGUSER, PGPASSWORD); with none set, psql's defaults. Creates the role `conduit`
# (password `conduit`) that your server connects as, with read/write access to the data and nothing else.
# The data is deterministic: the check at the end must print OK.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
if [ "${1:-}" = --dev ]; then SCALE=0.02; DB=conduit_dev; else SCALE=1.0; DB=conduit_seed; fi
DB="${SEED_DB:-$DB}"   # override the database name (testing)
DATA="$HERE/data-$SCALE"
PSQL="psql -X -q -v ON_ERROR_STOP=1"

# row counts and content fingerprints (sum over rows of the first 60 bits of md5(row::text), TimeZone UTC)
if [ "$SCALE" = 1.0 ]; then EXPECTED="
users 100000 57740453955909066846182
follows 1989808 1146981612039573987615992
articles 500000 288359265175553709847335
tags 200 107424899296369204925
article_tags 1250377 721107307953329306818644
favorites 4990637 2876715281728467325483444
comments 2494001 1436776691461277565723679"
else EXPECTED="
users 100000 57740453955909066846182
follows 39982 23042985450407282099700
articles 10000 5796775799610747246130
tags 200 107424899296369204925
article_tags 25048 14470473432458360801269
favorites 99911 57559556224651163688069
comments 49287 28499591671886831555647"
fi

echo "== seedgen -scale $SCALE -> $DATA"
(cd "$ROOT/bench" && go build -o "$HERE/.seedgen" ./cmd/seedgen)
rm -rf "$DATA"
"$HERE/.seedgen" -out "$DATA" -scale "$SCALE"

echo "== load into $DB"
t0=$(date +%s)
$PSQL -d postgres <<'SQL'
DO $$ BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'conduit') THEN CREATE ROLE conduit LOGIN PASSWORD 'conduit'; END IF;
END $$;
SQL
$PSQL -d postgres -c "DROP DATABASE IF EXISTS $DB WITH (FORCE)" -c "CREATE DATABASE $DB"
$PSQL -d "$DB" -f "$ROOT/schema.sql"
cp_in() {  # table, columns, file glob
  for f in $(ls "$DATA"/$3 | sort); do
    $PSQL -d "$DB" -c "\\copy $1 ($2) FROM '$f'"
  done
}
cp_in tags "id, name" "tags.tsv"
cp_in users "id, username, email, password_hash, bio, image, created_at" "users.tsv"
cp_in articles "id, slug, author_id, title, description, body, created_at, updated_at" "articles.*.tsv"
cp_in article_tags "article_id, tag_id" "article_tags.*.tsv"
cp_in favorites "user_id, article_id, created_at" "favorites.*.tsv"
cp_in comments "article_id, author_id, body, created_at, updated_at" "comments.*.tsv"
cp_in follows "follower_id, followee_id, created_at" "follows.*.tsv"
$PSQL -d "$DB" -o /dev/null <<'SQL'
SELECT setval(pg_get_serial_sequence('users', 'id'), (SELECT max(id) FROM users));
SELECT setval(pg_get_serial_sequence('articles', 'id'), (SELECT max(id) FROM articles));
SELECT setval(pg_get_serial_sequence('tags', 'id'), (SELECT max(id) FROM tags));
SELECT setval(pg_get_serial_sequence('comments', 'id'), (SELECT max(id) FROM comments));
GRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES IN SCHEMA public TO conduit;
GRANT USAGE, SELECT, UPDATE ON ALL SEQUENCES IN SCHEMA public TO conduit;
VACUUM (FREEZE, ANALYZE);
SQL
rm -rf "$DATA" "$HERE/.seedgen"
echo "loaded in $(( $(date +%s) - t0 )) s, $($PSQL -d "$DB" -Atc "SELECT pg_size_pretty(pg_database_size('$DB'))")"

echo "== check"
GOT=""
for t in users follows articles tags article_tags favorites comments; do
  GOT="$GOT
$t $($PSQL -d "$DB" -AtF ' ' -c "SET TimeZone = 'UTC'" -c "SELECT count(*), coalesce(sum(('x' || substr(md5(x::text), 1, 15))::bit(60)::bigint), 0) FROM $t x")"
done
if [ "$GOT" = "$EXPECTED" ]; then echo "OK: $DB matches the reference seed"
else echo "MISMATCH: got$GOT"; echo "expected$EXPECTED"; exit 1; fi
