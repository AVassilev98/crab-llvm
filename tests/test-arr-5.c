extern int nd ();
int a[10];
int b[10];

int main ()
{
  int i;
  for (i=0;i<10;i++)
  {
    if (nd ())
      a[i] =0;
    else 
      a[i] =5;
  }

  for (i=0;i<10;i++)
  {
    if (nd ())
      b[i] =20;
    else 
      b[i] =25;
  }

  int x = a[i-1]  + b[i-1];
  if (x > 0)
    return 42;
  else
    return 0;
}
