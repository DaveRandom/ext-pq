--TEST--
large objects
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
$t = $c->startTransaction();

$lob = $t->createLOB();

var_dump($lob->transaction === $t);

$data = file_get_contents(__FILE__);
$length = strlen($data);

$lob->write($data);
var_dump($lob->tell() === $length);

$lob->seek(0, SEEK_SET);
var_dump(md5($lob->read($length)) === md5($data));

$lob->truncate(5);

$lob = new pq\Lob($t, $lob->oid);
var_dump($lob->read(123));

$t->commit();
$t->unlinkLOB($lob->oid);

?>
DONE
--EXPECTF--
Test
bool(true)
bool(true)
bool(true)
string(5) "%c?php"
DONE

