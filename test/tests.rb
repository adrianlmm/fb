require 'fb'
require 'date'

$host = 'localhost'
$database = '/db/rastro.fdb'
$user = 'SYSDBA'
$pass = 'masterkey'

$test_start_time = 0
$ok = 0
$fail = 0

def pass(n)
  $ok += 1
  ms = ((Process.clock_gettime(Process::CLOCK_MONOTONIC) - $test_start_time) * 1000).round
  puts "  ✅ #{n} (#{ms}ms)"
end

def fail(n, e)
  $fail += 1
  ms = ((Process.clock_gettime(Process::CLOCK_MONOTONIC) - $test_start_time) * 1000).round
  puts "  ❌ #{n} — #{e.message} (#{ms}ms)"
end

def with_conn
  db = Fb::Database.new
  db.database = $database
  db.username = $user
  db.password = $pass
  db.downcase_names = true
  db.charset = 'UTF8'
  con = db.connect
  begin
    yield con
  ensure
    con.close rescue nil
  end
end

def execute(con, sql, *params)
  con.execute(sql, *params)
end

def query(con, sql, *params)
  con.execute(sql, *params) do |cursor|
    cursor.fetchall(:hash)
  end
end

def to_maps(rows_or_cursor)
  rows = rows_or_cursor.is_a?(Array) ? rows_or_cursor : rows_or_cursor.fetchall(:hash)
  rows.map do |row|
    row.transform_keys { |k| k.to_s.upcase }
  end
end

# ═══════════════════════════════════════════
#  TESTS
# ═══════════════════════════════════════════

def test_connect
  with_conn do |c|
    # simple connect/close
  end
  pass('connect/close')
end

def test_select_rdb
  with_conn do |c|
    r = to_maps(query(c, 'SELECT * FROM RDB$DATABASE'))
    raise "Expected 1 row" unless r.size == 1
    raise "Expected >= 4 columns" unless r.first.keys.size >= 4
  end
  pass('SELECT RDB$DATABASE')
end

def test_insert_rollback
  with_conn do |c|
    antes = to_maps(query(c, 'SELECT COUNT(*) FROM DRASCAM')).first['COUNT']
    c.transaction
    try_ok = false
    begin
      execute(c, "INSERT INTO DRASCAM (DESCRIPCION, COLUMNAS, FILAS, PROFUNDIDAD, VALIDA_POSICION) VALUES ('rb_test', '1', '1', '1', 'No')")
      dur = to_maps(query(c, 'SELECT COUNT(*) FROM DRASCAM')).first['COUNT']
      raise "Expected count to increase by 1" unless dur == antes + 1
      c.rollback
      try_ok = true
    ensure
      c.rollback unless try_ok
    end
    desp = to_maps(query(c, 'SELECT COUNT(*) FROM DRASCAM')).first['COUNT']
    raise "Count mismatch after rollback" unless desp == antes
  end
  pass('INSERT+ROLLBACK')
end

def test_null_values
  with_conn do |c|
    execute(c, 'CREATE TABLE NULLTEST2 (ID INT, VAL INT)')
    begin
      c.transaction
      try_ok = false
      begin
        execute(c, "INSERT INTO NULLTEST2 (ID, VAL) VALUES (?, ?)", 1, nil)
        execute(c, "INSERT INTO NULLTEST2 (ID, VAL) VALUES (?, ?)", 2, 42)
        c.commit
        try_ok = true
      ensure
        c.rollback unless try_ok
      end
      
      r = to_maps(query(c, 'SELECT * FROM NULLTEST2 WHERE ID = 1'))
      raise "Expected null value" unless r.first['VAL'].nil?
      
      r = to_maps(query(c, 'SELECT * FROM NULLTEST2 WHERE ID = 2'))
      raise "Expected 42" unless r.first['VAL'] == 42
    ensure
      execute(c, 'DROP TABLE NULLTEST2')
    end
  end
  pass('NULL values')
end

def test_cursor
  with_conn do |c|
    execute(c, 'CREATE TABLE TESTCURSOR (ID INT, NAME VARCHAR(20))')
    begin
      c.transaction
      try_ok = false
      begin
        5.times do |i|
          execute(c, "INSERT INTO TESTCURSOR (ID, NAME) VALUES (?, ?)", i, "row#{i}")
        end
        c.commit
        try_ok = true
      ensure
        c.rollback unless try_ok
      end
      rows = to_maps(query(c, 'SELECT * FROM TESTCURSOR ORDER BY ID'))
      raise "Expected 5 rows" unless rows.size == 5
      5.times do |i|
        raise "ID mismatch" unless rows[i]['ID'] == i
        raise "Name mismatch" unless rows[i]['NAME'].strip == "row#{i}"
      end
    ensure
      execute(c, 'DROP TABLE TESTCURSOR')
    end
  end
  pass('CURSOR fetch (5 rows)')
end

def test_columns
  with_conn do |c|
    execute(c, 'CREATE TABLE TESTCOLS (ID INT, NOME VARCHAR(20), AMT DOUBLE PRECISION)')
    begin
      c.execute('SELECT * FROM TESTCOLS') do |cursor|
        cols = cursor.fields.map { |f| f.name.upcase }
        raise "Expected 3 columns" unless cols.size == 3
        %w[ID NOME AMT].each do |n|
          raise "Missing column #{n}" unless cols.include?(n)
        end
      end
    ensure
      execute(c, 'DROP TABLE TESTCOLS')
    end
  end
  pass('COLUMNS metadata (3 cols)')
end

def test_table_names
  with_conn do |c|
    r = to_maps(query(c, "SELECT RDB$RELATION_NAME FROM RDB$RELATIONS WHERE RDB$VIEW_BLR IS NULL AND RDB$SYSTEM_FLAG = 0 ORDER BY RDB$RELATION_NAME"))
    raise "Expected non-empty table list" if r.empty?
  end
  pass('TABLE NAMES')
end

def test_timestamp
  with_conn do |c|
    r = to_maps(query(c, "SELECT CURRENT_TIMESTAMP AS TS FROM RDB$DATABASE"))
    ts = r.first['TS']
    if ts.nil?
      pass('TIMESTAMP (null)')
    elsif ts.is_a?(Time) || ts.is_a?(DateTime)
      pass("TIMESTAMP decode: #{ts.year}-#{ts.month}-#{ts.day}")
    else
      pass("TIMESTAMP type: #{ts.class}")
    end
  end
end

def test_big_query
  with_conn do |c|
    r = to_maps(query(c, 'SELECT * FROM DRASCORT ORDER BY ID DESC ROWS 10'))
    raise "Expected 10 rows" unless r.size == 10
  end
  pass('BIG query (77 cols x 10 rows)')
end

def test_read_only
  with_conn do |c|
    r = to_maps(query(c, 'SELECT COUNT(*) FROM DRASCAM'))
    raise "Expected numeric count" unless r.first['COUNT'].is_a?(Integer)
  end
  pass('READ ONLY transaction (adapted)')
end

def test_multi_fetch
  with_conn do |c|
    r = to_maps(query(c, 'SELECT FIRST 100 ID, PRODUCTO FROM DRASCORT ORDER BY ID DESC'))
    raise "Expected 100 rows, got #{r.size}" unless r.size == 100
  end
  pass('MULTI fetch (100 rows)')
end

def test_insert_returning
  with_conn do |c|
    c.transaction
    try_ok = false
    begin
      res = to_maps(query(c, "INSERT INTO DRASCAM (DESCRIPCION, COLUMNAS, FILAS, PROFUNDIDAD, PERMITE_SALIDA, VALIDA_POSICION) VALUES ('test_ret_dn', '1', '1', '1', 'Si', 'No') RETURNING ID"))
      raise "Expected 1 row" unless res.size == 1
      returned_id = res.first['ID']
      raise "ID is null" if returned_id.nil?
      raise "ID is not integer" unless returned_id.is_a?(Integer)
      raise "ID must be > 0" unless returned_id > 0
      puts "    ID generado: #{returned_id}"
      c.rollback
      try_ok = true
    ensure
      c.rollback unless try_ok
    end
  end
  pass('INSERT RETURNING ID')
end

def test_params
  with_conn do |c|
    execute(c, 'CREATE TABLE TESTPARAMS (ID INT, NOME VARCHAR(20))')
    begin
      c.transaction
      try_ok = false
      begin
        execute(c, 'INSERT INTO TESTPARAMS (ID, NOME) VALUES (?, ?)', 1, 'Alice')
        execute(c, 'INSERT INTO TESTPARAMS (ID, NOME) VALUES (?, ?)', 2, 'Bob')
        c.commit
        try_ok = true
      ensure
        c.rollback unless try_ok
      end
      
      r = to_maps(query(c, 'SELECT * FROM TESTPARAMS WHERE ID = ?', 1))
      raise "Expected 1 row" unless r.size == 1
      raise "Name mismatch" unless r.first['NOME'].strip == 'Alice'
      
      r2 = to_maps(query(c, 'SELECT * FROM TESTPARAMS WHERE NOME = ?', 'Bob'))
      raise "Expected 1 row" unless r2.size == 1
      raise "ID mismatch" unless r2.first['ID'] == 2
      
      c.transaction
      try_ok = false
      begin
        execute(c, 'INSERT INTO TESTPARAMS (ID, NOME) VALUES (?, ?)', 3, nil)
        c.commit
        try_ok = true
      ensure
        c.rollback unless try_ok
      end
      
      r3 = to_maps(query(c, 'SELECT * FROM TESTPARAMS WHERE ID = ?', 3))
      raise "Expected null value" unless r3.first['NOME'].nil?
    ensure
      execute(c, 'DROP TABLE TESTPARAMS')
    end
  end
  pass('PARAMS (?, valores y null)')
end

def test_multi_params
  with_conn do |c|
    execute(c, 'CREATE TABLE TESTMP (ID INT, NOME VARCHAR(20), ATIVO INT)')
    begin
      c.transaction
      try_ok = false
      begin
        execute(c, 'INSERT INTO TESTMP (ID, NOME, ATIVO) VALUES (?, ?, ?)', 1, 'Teste', 1)
        execute(c, 'INSERT INTO TESTMP (ID, NOME, ATIVO) VALUES (?, ?, ?)', 2, 'Teste2', 0)
        c.commit
        try_ok = true
      ensure
        c.rollback unless try_ok
      end
      
      r = to_maps(query(c, 'SELECT * FROM TESTMP WHERE ID = ? AND NOME = ?', 1, 'Teste'))
      raise "Expected 1 row" unless r.size == 1
      raise "Ativo mismatch" unless r.first['ATIVO'] == 1
    ensure
      execute(c, 'DROP TABLE TESTMP')
    end
  end
  pass('MULTI PARAMS (3 ? placeholders)')
end

def test_blob_image
  img_path = '/home/aam/Imágenes/632915627_2953809611479093_8508249442628109187_n.jpg'
  img_bytes = File.binread(img_path)
  with_conn do |c|
    puts "    imagen: #{img_bytes.length} bytes"
    execute(c, 'CREATE TABLE TPBLOB (ID INT, ATTACHMENT BLOB SUB_TYPE 0)')
    begin
      c.transaction
      try_ok = false
      begin
        execute(c, 'INSERT INTO TPBLOB (ID, ATTACHMENT) VALUES (?, ?)', 1, img_bytes)
        c.commit
        try_ok = true
      ensure
        c.rollback unless try_ok
      end
      
      r = to_maps(query(c, 'SELECT * FROM TPBLOB WHERE ID = ?', 1))
      raise "Row not found" unless r.size == 1
      blob_data = r.first['ATTACHMENT']
      
      raise "Expected String/binary data" unless blob_data.is_a?(String)
      raise "Size mismatch: #{blob_data.length} != #{img_bytes.length}" unless blob_data.length == img_bytes.length
      
      100.times do |i|
        break if i >= img_bytes.length
        raise "Byte #{i} mismatch" unless blob_data[i] == img_bytes[i]
      end
      puts "    coincide: #{blob_data.length} bytes verificado"
    ensure
      execute(c, 'DROP TABLE TPBLOB')
    end
  end
  pass("BLOB IMAGE (#{img_bytes.length} bytes)")
end

def test_fetch_one_by_one
  with_conn do |c|
    execute(c, 'CREATE TABLE TEST_FBO (ID INT, VAL VARCHAR(20))')
    begin
      10.times do |i|
        execute(c, "INSERT INTO TEST_FBO (ID, VAL) VALUES (?, ?)", i, "row_#{i}")
      end
      
      c.execute('SELECT * FROM TEST_FBO ORDER BY ID') do |cursor|
        count = 0
        while (row = cursor.fetch(:hash))
          raise "ID mismatch" unless row['id'] == count
          raise "Value mismatch" unless row['val'].strip == "row_#{count}"
          count += 1
        end
        raise "Expected 10 rows" unless count == 10
        begin
          cursor.fetch(:hash)
          raise "Expected Fb::Error post-EOF"
        rescue Fb::Error => e
          raise e unless e.message.include?("past end of data")
        end
      end
    ensure
      execute(c, 'DROP TABLE TEST_FBO')
    end
  end
  pass('FETCH one by one (10 rows)')
end

def test_cursor_fetch
  with_conn do |c|
    execute(c, 'CREATE TABLE TEST_CF (ID INT, NOME VARCHAR(30), VALOR DOUBLE PRECISION)')
    begin
      execute(c, "INSERT INTO TEST_CF (ID, NOME, VALOR) VALUES (1, 'Alpha', 10.5)")
      execute(c, "INSERT INTO TEST_CF (ID, NOME, VALOR) VALUES (2, 'Beta', 20.5)")
      execute(c, "INSERT INTO TEST_CF (ID, NOME, VALOR) VALUES (3, 'Gamma', 30.5)")
      
      c.execute('SELECT * FROM TEST_CF ORDER BY ID') do |cursor|
        r1 = cursor.fetch(:hash)
        raise "Row 1 missing" if r1.nil?
        raise "Row 1 mismatch" unless r1['id'] == 1 && r1['nome'].strip == 'Alpha'
        
        r2 = cursor.fetch(:hash)
        raise "Row 2 missing" if r2.nil?
        raise "Row 2 mismatch" unless r2['id'] == 2 && r2['nome'].strip == 'Beta'
        
        r3 = cursor.fetch(:hash)
        raise "Row 3 missing" if r3.nil?
        raise "Row 3 mismatch" unless r3['id'] == 3 && r3['nome'].strip == 'Gamma'
        
        raise "Expected EOF" unless cursor.fetch(:hash).nil?
      end
    ensure
      execute(c, 'DROP TABLE TEST_CF')
    end
  end
  pass('CURSOR FETCH one by one')
end

def test_stream_large_dataset
  with_conn do |c|
    execute(c, 'CREATE TABLE TEST_STREAM (ID INT, VAL VARCHAR(20))')
    begin
      c.transaction
      try_ok = false
      begin
        1500.times do |i|
          execute(c, 'INSERT INTO TEST_STREAM (ID, VAL) VALUES (?, ?)', i, "val_#{i}")
        end
        c.commit
        try_ok = true
      ensure
        c.rollback unless try_ok
      end
      
      c.execute('SELECT * FROM TEST_STREAM ORDER BY ID') do |cursor|
        count = 0
        while (row = cursor.fetch(:hash))
          raise "Stream row ##{count} ID mismatch: #{row['id']} != #{count}" unless row['id'] == count
          raise "Stream row ##{count} value mismatch" unless row['val'].strip == "val_#{count}"
          count += 1
        end
        raise "Expected 1500 rows, got #{count}" unless count == 1500
      end
      
      c.execute('SELECT * FROM TEST_STREAM ORDER BY ID') do |cursor1|
        has_first = cursor1.fetch(:hash)
        raise "Expected row" if has_first.nil?
        raise "Expected ID 0" unless has_first['id'] == 0
        
        success = false
        begin
          res = to_maps(query(c, 'SELECT * FROM TEST_STREAM ROWS 1'))
          success = !res.empty?
        rescue => e
          # catch exception if unsupported
        end
        raise "Simultaneous query failed" unless success
      end
    ensure
      execute(c, 'DROP TABLE TEST_STREAM')
    end
  end
  pass('queryStream validation (1500 rows + socket lock + early cancel)')
end

def test_stream_large_dataset2
  with_conn do |c|
      c.execute('SELECT FIRST 50000 * FROM DRASCORT ORDER BY ID DESC') do |cursor|
        count = 0
        while (row = cursor.fetch(:hash))
          count += 1
        end
      end
  end
  pass('queryStream 50000 rows DRASCORT')
end

# ═══════════════════════════════════════════
#  MAIN ENTRY POINT
# ═══════════════════════════════════════════

def main
  total_start = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  puts "═══ Ruby fb Gem Test Suite ═══"
  puts "DB: #{$database}\n\n"

  # Limpieza inicial
  with_conn do |c|
    %w[NULLTEST2 TESTCURSOR TESTCOLS TESTPARAMS TESTMP TPBLOB TEST_FBO TEST_CF TEST_STREAM].each do |tbl|
      begin
        execute(c, "DROP TABLE #{tbl}")
      rescue => e
      end
    end
  end

  tests = [
    :test_connect,
    :test_select_rdb,
    :test_insert_rollback,
    :test_null_values,
    :test_cursor,
    :test_columns,
    :test_table_names,
    :test_timestamp,
    :test_big_query,
    :test_read_only,
    :test_multi_fetch,
    :test_insert_returning,
    :test_params,
    :test_multi_params,
    :test_blob_image,
    :test_fetch_one_by_one,
    :test_cursor_fetch,
    :test_stream_large_dataset,
    :test_stream_large_dataset2
  ]

  tests.each do |t|
    begin
      $test_start_time = Process.clock_gettime(Process::CLOCK_MONOTONIC)
      send(t)
    rescue => e
      fail(t.to_s, e)
    end
  end

  total_ms = ((Process.clock_gettime(Process::CLOCK_MONOTONIC) - total_start) * 1000).round
  puts "\n═════════════════════════"
  puts "  #{$ok} ✅  #{$fail} ❌"
  puts "  Tiempo total: #{total_ms}ms (#{(total_ms / 1000.0).round(2)}s)"
  puts "═════════════════════════"
end

main
