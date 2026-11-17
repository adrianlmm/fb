require 'fb'
include Fb

def obteber_conexion
  begin
    conexion = Fb::Database.new
    conexion.database = "/db/rastro.fdb"
    conexion.username = "SYSDBA"
    conexion.password = "masterkey"
    conexion.downcase_names = true
    conexion.charset = "UTF8"     
    return conexion.connect
  rescue Fb::Error => e
    puts "Error al conectar a la base de datos: #{e.message}"
    exit(1)
  end
end

# Inserta +cantidad+ registros en DRASCAM usando el metodo multi-tupla:
# una sola sentencia preparada (con place holders ?) y una sola llamada a execute
# con un array de tuplas, en lugar de abrir/cerrar una conexion por registro.
def insertar_registros_multitupla(con, cantidad = 10)
  sentencia = "INSERT INTO DRASCAM " \
              "(DESCRIPCION, COLUMNAS, FILAS, PROFUNDIDAD, PERMITE_SALIDA, VALIDA_POSICION) " \
              "VALUES (?, ?, ?, ?, ?, ?)"
  registros = (1..cantidad).map do |i|
    ["Multitupla #{i}", "1", "1", "1", "Si", "No"]
  end

  # Ojo: con multi-tupla execute() devuelve las filas afectadas reportadas por el
  # driver para la sentencia (1), NO la cantidad de registros enviados. Por eso
  # informamos tambien cuantos registros se enviaron.
  afectadas = con.execute(sentencia, registros)
  puts "Registros enviados con multitupla: #{registros.size} (filas afectadas segun execute: #{afectadas})"
  afectadas
end

con = obteber_conexion

p_sentencia = "INSERT INTO DRASCAM (DESCRIPCION, COLUMNAS, FILAS, PROFUNDIDAD, PERMITE_SALIDA, VALIDA_POSICION) VALUES ('Defaul2', '1', '1', '1', 'Si', 'No') RETURNING *";
data = con.execute(p_sentencia)

puts data.fetch

insertar_registros_multitupla(con, 10000)

#p_sentencia = "SELECT LOCALTIMESTAMP FROM RDB$DATABASE"
#rows = con.query(p_sentencia)
#puts "Resultado: #{rows.inspect}"

con.close;
