cmd_/home/ubuntu/Documents/SOA/project/Module.symvers := sed 's/\.ko$$/\.o/' /home/ubuntu/Documents/SOA/project/modules.order | scripts/mod/modpost -m -a  -o /home/ubuntu/Documents/SOA/project/Module.symvers -e -i Module.symvers   -T -
