require 'test/FbTestCases'

class CursorTestCases < FbTestCase
  include FbTestCases

  def test_fetch_array
    Database.create(@parms) do |connection|
      connection.execute("select * from rdb$database") do |cursor|
        assert_instance_of Cursor, cursor
        row = cursor.fetch :array
        assert_instance_of Array, row
        assert_equal expected_columns, row.size
      end
      connection.execute("select * from rdb$database where rdb$description = 'bogus'") do |cursor|
        assert_instance_of Cursor, cursor
        row = cursor.fetch :array
        assert_instance_of NilClass, row
        assert_nil row
      end
      connection.drop
    end
  end

  def test_fetch_hash
    Database.create(@parms) do |connection|
      connection.execute("select * from rdb$database") do |cursor|
        assert_instance_of Cursor, cursor
        row = cursor.fetch :hash
        assert_instance_of Hash, row
        assert_equal expected_columns, row.size
      end
      connection.execute("select * from rdb$database where rdb$description = 'bogus'") do |cursor|
        assert_instance_of Cursor, cursor
        row = cursor.fetch :hash
        assert_instance_of NilClass, row
        assert_nil row
      end
      connection.drop
    end
  end

  def test_fetch_all_array
    Database.create(@parms) do |connection|
      connection.execute("select * from rdb$database") do |cursor|
        assert_instance_of Cursor, cursor
        rows = cursor.fetchall :array
        assert_instance_of Array, rows
        assert_equal 1, rows.size
        assert_instance_of Array, rows[0]
        assert_equal expected_columns, rows[0].size
      end
      connection.drop
    end
  end

  def test_fetch_all_hash
    Database.create(@parms) do |connection|
      connection.execute("select * from rdb$database") do |cursor|
        assert_instance_of Cursor, cursor
        rows = cursor.fetchall :hash
        assert_instance_of Array, rows
        assert_equal 1, rows.size
        assert_instance_of Hash, rows[0]
        assert_equal expected_columns, rows[0].size
      end
      connection.drop
    end
  end

  def test_fields_array
    Database.create(@parms) do |connection|
      connection.execute("select * from rdb$database") do |cursor|
        fields = cursor.fields
        fields_ary = cursor.fields :array
        assert_equal fields, fields_ary
        assert_equal expected_columns, fields.size
        assert_equal "RDB$DESCRIPTION", fields[0].name;
        assert_equal "RDB$RELATION_ID", fields[1].name;
        assert_equal "RDB$SECURITY_CLASS", fields[2].name;
        assert_equal "RDB$CHARACTER_SET_NAME", fields[3].name;
        assert_equal "RDB$LINGER", fields[4].name if @fb_version >= 3
      end
      connection.drop
    end
  end

  def test_fields_array_downcased
    Database.create(@parms.merge(:downcase_names => true)) do |connection| # xxx true
      connection.execute("select * from rdb$database") do |cursor|
        fields = cursor.fields
        fields_ary = cursor.fields :array
        assert_equal fields, fields_ary
        assert_equal expected_columns, fields.size
        assert_equal "rdb$description", fields[0].name;
        assert_equal "rdb$relation_id", fields[1].name;
        assert_equal "rdb$security_class", fields[2].name;
        assert_equal "rdb$character_set_name", fields[3].name;
        assert_equal "rdb$linger", fields[4].name if @fb_version >= 3
      end
      connection.drop
    end
  end

  def test_fields_hash
    Database.create(@parms) do |connection|
      connection.execute("select * from rdb$database") do |cursor|
        fields = cursor.fields :hash
        assert_equal expected_columns, fields.size
        assert_equal 520, fields["RDB$DESCRIPTION"].type_code
        assert_equal 500, fields["RDB$RELATION_ID"].type_code
        assert_equal 452, fields["RDB$SECURITY_CLASS"].type_code
        assert_equal 452, fields["RDB$CHARACTER_SET_NAME"].type_code
        assert_equal 496, fields["RDB$LINGER"].type_code if @fb_version >= 3
      end
      connection.drop
    end
  end

  def test_fields_hash_downcased
    Database.create(@parms.merge(:downcase_names => true)) do |connection| # xxx true
      connection.execute("select * from rdb$database") do |cursor|
        fields = cursor.fields :hash
        assert_equal expected_columns, fields.size
        assert_equal 520, fields["rdb$description"].type_code
        assert_equal 500, fields["rdb$relation_id"].type_code
        assert_equal 452, fields["rdb$security_class"].type_code
        assert_equal 452, fields["rdb$character_set_name"].type_code
        assert_equal 496, fields["rdb$linger"].type_code if @fb_version >= 3
      end
      connection.drop
    end
  end

  def test_each_array
    Database.create(@parms) do |connection|
      connection.execute("select * from rdb$database") do |cursor|
        count = 0
        cursor.each :array do |row|
          count += 1
          assert_instance_of Array, row
          assert_equal expected_columns, row.size
        end
        assert_equal 1, count
      end
      connection.drop
    end
  end

  def test_each_hash
    Database.create(@parms) do |connection|
      connection.execute("select * from rdb$database") do |cursor|
        count = 0
        cursor.each :hash do |row|
          count += 1
          assert_instance_of Hash, row
          assert_equal expected_columns, row.size
        end
        assert_equal 1, count
      end
      connection.drop
    end
  end

  def test_fetch_after_nil
    Database.create(@parms) do |connection|
      connection.execute("create generator test_seq");
      connection.execute("select gen_id(test_seq, 1) from rdb$database") do |cursor|
        r1 = cursor.fetch
        assert !r1.nil?
        r2 = cursor.fetch
        assert_nil r2
        assert_raises Error do
          r3 = cursor.fetch
        end
      end
      connection.execute("select * from rdb$database") do |cursor|
        r1 = cursor.fetch
        assert !r1.nil?
        r2 = cursor.fetch
        assert_nil r2
        assert_raises Error do
          r3 = cursor.fetch
        end
      end
      connection.drop
    end
  end

  def test_fetch_hash_with_aliased_fields
    sql = "SELECT RDB$DESCRIPTION DES, RDB$RELATION_ID REL, RDB$SECURITY_CLASS SEC, RDB$CHARACTER_SET_NAME FROM RDB$DATABASE"
    Database.create(@parms) do |connection|
      connection.execute(sql) do |cursor|
        assert_instance_of Cursor, cursor
        row = cursor.fetch :hash
        assert_instance_of Hash, row
        assert_equal 4, row.size
        assert row.keys.include?("DES"), "No field DES"
        assert row.keys.include?("REL"), "No field REL"
        assert row.keys.include?("SEC"), "No field SEC"
        assert row.keys.include?("RDB$CHARACTER_SET_NAME"), "No field RDB$CHARACTER_SET_NAME"
      end
      connection.drop
    end
  end
  
  def test_simultaneous_cursors
    sql_schema = <<-END
      CREATE TABLE MASTER (ID INT, NAME1 VARCHAR(10));
      CREATE TABLE DETAIL (ID INT, MASTER_ID INT, NAME2 VARCHAR(10));
    END
    sql_insert_master = "INSERT INTO MASTER (ID, NAME1) VALUES (?, ?)"
    sql_insert_detail = "INSERT INTO DETAIL (ID, MASTER_ID, NAME2) VALUES (?, ?, ?)"
    sql_select_master = "SELECT * FROM MASTER ORDER BY ID"
    sql_select_detail = "SELECT * FROM DETAIL ORDER BY ID"
    Database.create(@parms) do |connection|
      connection.execute_script(sql_schema)
      connection.transaction do
        3.times do |m|
          connection.execute(sql_insert_master, m, "name_#{m}")
        end
        9.times do |d|
          connection.execute(sql_insert_detail, d, d / 3, "name_#{d / 3}_#{d}")
        end
      end
      master = connection.execute(sql_select_master)
      begin
        detail = connection.execute(sql_select_detail)
        begin
          3.times do |m|
            mr = master.fetch
            assert_equal m, mr[0]
            assert_equal "name_#{m}", mr[1]
            3.times do |d|
              dr = detail.fetch
              assert_equal m * 3 + d, dr[0]
              assert_equal m, dr[1]
              assert_equal "name_#{m}_#{m * 3 + d}", dr[2]
            end
          end
        ensure
          detail.close
        end
      ensure
        master.close
      end
    end
  end

  # Un SELECT con mas de SQLDA_COLSINIT (50) columnas obliga a reasignar el SQLDA
  # de salida. La cache de sentencias se quedaba con el puntero al SQLDA viejo
  # (ya liberado), asi que a partir de la SEGUNDA ejecucion del mismo SQL se leia
  # memoria liberada: `query` devolvia 0 en lugar de las filas (o el proceso se
  # caia con doble free / SIGSEGV).
  # Se repite el mismo SQL a proposito (las ejecuciones siguientes vienen de la
  # cache). Con 1000 columnas y 10 vueltas el fallo se ve de forma segura: la
  # segunda vuelta ya devuelve 0 en lugar de las filas.
  # Ademas, al final se ejecuta un DDL (DROP TABLE): la invalidacion de la cache
  # hace DSQL_drop de la sentencia cacheada y Firebird escribe NULL, no solo en la
  # variable que se le pasa, sino tambien en la direccion del campo stmt que se le
  # dio al prepararla (la del cursor dueno). Si ese cursor ya lo habia liberado el
  # GC y Ruby habia reciclado su memoria para otro cursor, el handle del nuevo
  # quedaba a cero -> "invalid statement handle". Por eso el dueno queda anclado
  # (fb_stmt_cache_destroy) hasta destruir la entrada.
  def test_more_than_50_columns_repeated_query
    columns = 1000
    runs = 10
    definitions = (1..columns).map { |i| "C#{i} INTEGER" }.join(', ')
    names = (1..columns).map { |i| "C#{i}" }.join(', ')
    values = (1..columns).map { |i| i.to_s }.join(', ')
    sql_select = "SELECT * FROM TEST_WIDE"
    expected = (1..columns).to_a

    Database.create(@parms) do |connection|
      connection.execute("CREATE TABLE TEST_WIDE (#{definitions})")
      connection.execute("INSERT INTO TEST_WIDE (#{names}) VALUES (#{values})")
      connection.commit

      runs.times do |run|
        rows = connection.query(sql_select)
        assert_instance_of Array, rows, "run #{run}"
        assert_equal 1, rows.size, "run #{run}"
        assert_equal columns, rows.first.size, "run #{run}"
        assert_equal expected, rows.first, "run #{run}"
      end

      connection.execute("DROP TABLE TEST_WIDE")
      connection.drop
    end
  end
end
