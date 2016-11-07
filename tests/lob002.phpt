--TEST--
large object stream
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
$t = $c->startTransaction();

$data = file_get_contents(__FILE__);
$length = strlen($data);

$lob = $t->createLOB();
fwrite($lob->stream, $data);
var_dump(ftell($lob->stream) === $length);

fseek($lob->stream, 0, SEEK_SET);
var_dump(md5(fread($lob->stream, $length)) === md5($data));

ftruncate($lob->stream, 5);

$lob = new pq\Lob($t, $lob->oid);
var_dump(fread($lob->stream, 123) === substr($data, 0, 123));

$t->commit();
$t->unlinkLOB($lob->oid);

?>
DONE
--EXPECTF--
Test
bool(true)
bool(true)

Warning: ftruncate(): Can't truncate this stream! in %s on line %d
bool(true)
DONE

