// Spec 093 T4: two user CLR types for test/sql/types/clr_udt_raw_scan.test -- a raw mssql_scan reads a
// user CLR type as a BLOB of its stored bytes, the catalog through CAST(... AS NVARCHAR(MAX)), its
// ToString(). Synthetic, built by test/clr/build.sh; never deployed anywhere but a test server.
//
// TestPoint: Format.Native, 9 bytes (a null flag and two ints) -- a small MAX_BYTE_SIZE in UDT_INFO.
// TestLabel: Format.UserDefined, MaxByteSize = -1 -- UDT_INFO's 0xFFFF, a value past 8000 bytes.

using System;
using System.Data.SqlTypes;
using System.Globalization;
using System.IO;
using Microsoft.SqlServer.Server;

namespace MssqlTest
{
	[Serializable]
	[SqlUserDefinedType(Format.Native, IsByteOrdered = true)]
	public struct TestPoint : INullable
	{
		private bool is_null;
		private int x;
		private int y;

		public bool IsNull
		{
			get { return is_null; }
		}

		public static TestPoint Null
		{
			get
			{
				TestPoint p = new TestPoint();
				p.is_null = true;
				return p;
			}
		}

		public int X
		{
			get { return x; }
		}

		public int Y
		{
			get { return y; }
		}

		public override string ToString()
		{
			return is_null ? "NULL" : x.ToString(CultureInfo.InvariantCulture) + "," + y.ToString(CultureInfo.InvariantCulture);
		}

		public static TestPoint Parse(SqlString s)
		{
			if (s.IsNull)
			{
				return Null;
			}
			string[] parts = s.Value.Split(',');
			if (parts.Length != 2)
			{
				throw new ArgumentException("TestPoint: 'x,y'");
			}
			TestPoint p = new TestPoint();
			p.x = int.Parse(parts[0].Trim(), CultureInfo.InvariantCulture);
			p.y = int.Parse(parts[1].Trim(), CultureInfo.InvariantCulture);
			return p;
		}
	}

	[Serializable]
	[SqlUserDefinedType(Format.UserDefined, MaxByteSize = -1)]
	public class TestLabel : INullable, IBinarySerialize
	{
		private string value;

		public bool IsNull
		{
			get { return value == null; }
		}

		public static TestLabel Null
		{
			get { return new TestLabel(); }
		}

		public override string ToString()
		{
			return value ?? "NULL";
		}

		public static TestLabel Parse(SqlString s)
		{
			TestLabel label = new TestLabel();
			if (!s.IsNull)
			{
				label.value = s.Value;
			}
			return label;
		}

		public void Read(BinaryReader r)
		{
			value = r.ReadString();
		}

		public void Write(BinaryWriter w)
		{
			w.Write(value ?? "");
		}
	}
}
